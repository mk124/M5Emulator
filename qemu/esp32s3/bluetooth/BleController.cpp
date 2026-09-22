/**
 * Copyright (C) 2026 MK124 and contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "BleController.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <utility>

#include "BleBytes.hpp"

namespace m5emulator::ble {

static constexpr uint32_t OutOfMemory = 0x101, InvalidArgument = 0x102, InvalidState = 0x103, NotSupported = 0x106;
static constexpr uint32_t WorkspaceSize = 512, TaskHandleOffset = 16, PacketOffset = 32;

// Shared callback prefix of ESP32-S3 controller OSI versions 0x00010007 / 0x0001000A / 0x0001000B.
static constexpr size_t OsiSemaphoreCreate = 0x24 / 4, OsiSemaphoreDelete = 0x28 / 4, OsiSemaphoreTake = 0x34 / 4;
static constexpr size_t OsiTaskCreate = 0x64 / 4, OsiTaskDelete = 0x68 / 4, OsiMallocInternal = 0x78 / 4, OsiFree = 0x7C / 4;
static constexpr uint32_t RomOsiPointer = 0x3FCEFF60;

static BleHleStep returnValue(uint32_t result = 0) { return { HleReturn, 0, {}, result }; }

// CALL8 returns the callee a2 in caller a10; intercepted API arguments are a2 onward.
static BleHleStep call(uint32_t target, uint32_t continuation, std::initializer_list<uint32_t> arguments)
{
	BleHleStep step { target, continuation, {}, 0 };
	std::copy(arguments.begin(), arguments.end(), step.args);
	return step;
}

BleHleStep BleController::step(void* guest, uint32_t operation, std::span<const uint32_t, 16> registers)
{
	switch (operation)
	{
		case HleInit: {
			if (_status || _workspace) return returnValue(InvalidState);
			if (!registers[2]) return returnValue(InvalidArgument);
			std::array<uint8_t, 16> config {};
			bleHleRead(guest, registers[2], config.data(), config.size());
			const uint32_t version = readLe32(std::span(config).subspan(4));
			if (readLe32(config) != 0x5A5AA5A5 || (version != 0x02401120 && version != 0x02509280) || config[12] != 1)
			{
				return returnValue(InvalidArgument);
			}
			
			_workerStackBytes = readLe16(std::span(config).subspan(8));
			_workerPriority = config[10];
			_workerCore = config[11];
			_sleepMode = config[14];
			
			uint32_t osiAddress = 0;
			bleHleRead(guest, RomOsiPointer, &osiAddress, sizeof(osiAddress));
			bleHleRead(guest, osiAddress, _osi.data(), sizeof(_osi));
			const bool supportedOsi = version == 0x02401120 ? _osi[1] == 0x00010007 : (_osi[1] == 0x0001000A || _osi[1] == 0x0001000B);
			if (_osi[0] != 0xFADEBEAD || !supportedOsi) return returnValue(NotSupported);
			for (const size_t slot : { OsiSemaphoreCreate, OsiSemaphoreDelete, OsiSemaphoreTake, OsiTaskCreate, OsiTaskDelete, OsiMallocInternal, OsiFree })
			{
				std::array<uint8_t, 3> entry {};
				if (!bleHleTryRead(guest, _osi[slot], entry.data(), entry.size()) || entry[0] != 0x36 || (entry[1] & 15) != 1) return returnValue(NotSupported);
			}
			
			std::fprintf(stderr, "BLE HLE: controller OSI=%08X, task core=%u priority=%u stack=%u\n", osiAddress, _workerCore, _workerPriority, _workerStackBytes);
			return call(_osi[OsiMallocInternal], HleAllocated, { WorkspaceSize });
		}
		case HleAllocated: {
			_workspace = registers[10];
			if (!_workspace) return returnValue(OutOfMemory);
			std::array<uint8_t, WorkspaceSize> bytes {};
			std::memcpy(bytes.data(), "ble-hle", 8);
			bleHleWrite(guest, _workspace, bytes.data(), bytes.size());
			return call(_osi[OsiSemaphoreCreate], HleSemaphoreCreated, { 1, 0 });
		}
		case HleSemaphoreCreated: {
			_semaphore = registers[10];
			if (!_semaphore) return call(_osi[OsiFree], HleTaskCreateFailed, { _workspace });
			return call(_osi[OsiTaskCreate], HleTaskCreated, { HleTaskEntry, _workspace, _workerStackBytes, _workspace, _workerPriority, _workspace + TaskHandleOffset, _workerCore });
		}
		case HleTaskCreated: {
			if (registers[10] != 1)
			{
				_initFailed = true;
				return call(_osi[OsiSemaphoreDelete], HleSemaphoreDeleted, { _semaphore });
			}
			
			bleHleRead(guest, _workspace + TaskHandleOffset, &_task, sizeof(_task));
			_status = 1;
			std::fprintf(stderr, "BLE HLE: INITED task=%08X workspace=%08X\n", _task, _workspace);
			return returnValue();
		}
		case HleTaskCreateFailed: {
			*this = {};
			return returnValue(OutOfMemory);
		}
		case HleEnable: {
			if (_status != 1) return returnValue(InvalidState);
			if (registers[2] != 1) return returnValue(InvalidArgument);
			
			_status = 2;
			std::fprintf(stderr, "BLE HLE: ENABLED\n");
			return returnValue();
		}
		case HleDisable: {
			// btdm_controller_disable is void; the SDK wrapper has already checked its status.
			_status = 1;
			resetHci();
			_events.clear();
			_readyPending = _commandPending = false;
			_sendAvailable = true;
			_completedAcl = 0;
			_readyCallback = _receiveCallback = 0;
			// A guest task may disable from inside, or preempt, the receive callback.
			// Keep its workspace intact, and never pop a new session's event when it returns.
			_receiveCancelled = _receiveInFlight;
			std::fprintf(stderr, "BLE HLE: DISABLED\n");
			return returnValue();
		}
		case HleDeinit: {
			if (_status != 1) return returnValue(InvalidState);
			return call(_osi[OsiTaskDelete], HleTaskDeleted, { _task });
		}
		case HleTaskDeleted:
			return call(_osi[OsiSemaphoreDelete], HleSemaphoreDeleted, { _semaphore });
		case HleSemaphoreDeleted: {
			_semaphore = 0;
			return call(_osi[OsiFree], _initFailed ? HleTaskCreateFailed : HleFreed, { _workspace });
		}
		case HleFreed: {
			std::fprintf(stderr, "BLE HLE: IDLE commands=%u received=%u ready=%u delay-resumes=%u\n", _commands, _received, _ready, _delayResumes);
			*this = {};
			return returnValue();
		}
		case HleMode:
			return returnValue(_status ? 1 : 0);
		case HlePowerActive:
			return returnValue(_status != 0);
		case HleSleepMode:
			return returnValue(_sleepMode);
		case HleSleepEnable: {
			_sleepEnabled = registers[2] != 0;
			return returnValue();
		}
		case HleWakeupRequesting: {
			_wakeupRequested = registers[2] != 0;
			return returnValue();
		}
		case HleWakeup: {
			_wakeupRequested = true;
			return returnValue();
		}
		case HlePhyInit: {
			_phyEnabled = true;
			return returnValue();
		}
		case HlePhyWakeup:
		case HlePhyWakeupImpl: {
			_phyEnabled = true;
			return returnValue();
		}
		case HlePhyClose:
		case HlePhyCloseImpl: {
			_phyEnabled = false;
			return returnValue();
		}
		case HlePhyTrack:
		case HlePhyTemperatureOff:
			return returnValue();
		case HleOffloadRegister: {
			if (registers[2] >= _offloadCallbacks.size() || !registers[3]) return returnValue(-1);
			_offloadCallbacks[registers[2]] = registers[3];
			return returnValue();
		}
		case HleOffloadDeregister: {
			if (registers[2] >= _offloadCallbacks.size()) return returnValue(-1);
			_offloadCallbacks[registers[2]] = 0;
			return returnValue();
		}
		case HlePllTrack:
		case HleAdvFlowControl:
		case HleClearLegacyAdv:
		case HleDuplicateExceptions:
		case HleChannelSelection:    {
			_features[operation - HlePllTrack] = registers[2] != 0;
			return returnValue();
		}
		case HleRegister: {
			if (_status != 2) return returnValue(InvalidState);
			if (!registers[2]) return returnValue(InvalidArgument);
			
			std::array<uint32_t, 2> callbacks {};
			bleHleRead(guest, registers[2], callbacks.data(), sizeof(callbacks));
			_readyCallback = callbacks[0];
			_receiveCallback = callbacks[1];
			std::fprintf(stderr, "BLE HLE: VHCI callbacks ready=%08X receive=%08X\n", _readyCallback, _receiveCallback);
			return returnValue();
		}
		case HleAvailable:
			return returnValue(_status == 2 && _sendAvailable);
		case HleSend: {
			if (_status != 2 || !_sendAvailable) fail("send while controller unavailable");
			send(guest, registers[2], registers[3]);
			return returnValue();
		}
		case HlePowerSet: {
			if (_status != 2 || registers[2] > 4 || registers[4] > 15) return returnValue(-1);
			if (registers[2] == 4)
			{
				if (!_connection || registers[3] != _connection) return returnValue(-1);
				_connectionPower = registers[4];
			}
			else if (registers[2] == 0) _defaultPower = registers[4];
			else if (registers[2] == 1 && (registers[3] == 0 || registers[3] == 0xFFFF)) _advertisingPower = registers[4];
			else return returnValue(-1);
			return returnValue();
		}
		case HlePowerGet: {
			if (_status != 2) return returnValue(-1);
			if (registers[2] == 0) return returnValue(_defaultPower);
			if (registers[2] == 1 && (registers[3] == 0 || registers[3] == 0xFFFF)) return returnValue(_advertisingPower.value_or(_defaultPower));
			if (registers[2] == 4 && registers[3] == _connection && _connection) return returnValue(_connectionPower);
			return returnValue(-1);
		}
		case HleTaskPoll:
			return poll(guest);
		case HleDelayReturned: {
			// OSI rounds milliseconds down to ticks. Repeated immediate returns indicate a zero-tick wait.
			_shortWaits = bleHleTimeNs() - _delayStartedNs < 100'000 ? _shortWaits + 1 : 0;
			if (_shortWaits == 8)
			{
				if (_pollTimeoutMs >= 1000) fail("OSI semaphore wait did not block");
				_pollTimeoutMs = std::min(_pollTimeoutMs * 2, 1000u);
				_shortWaits = 0;
			}
			++_delayResumes;
			return poll(guest);
		}
		case HleReceiveReturned: {
			_receiveInFlight = false;
			if (std::exchange(_receiveCancelled, false)) return poll(guest);
			if (registers[10] != 0)
			{
				// The host rejected this delivery; keep the packet and yield before retrying.
				_delayStartedNs = bleHleTimeNs();
				return call(_osi[OsiSemaphoreTake], HleDelayReturned, { _semaphore, _pollTimeoutMs });
			}
			_events.pop_front();
			++_received;
			return poll(guest);
		}
		case HleReadyReturned: {
			++_ready;
			return poll(guest);
		}
		default:
			fail("unexpected model continuation");
	}
}

BleHleStep BleController::poll(void* guest)
{
	pollPeer();
	if (_status != 2 || !_receiveCallback || !_readyCallback)
	{
		_delayStartedNs = bleHleTimeNs();
		return call(_osi[OsiSemaphoreTake], HleDelayReturned, { _semaphore, _pollTimeoutMs });
	}
	if (_readyPending)
	{
		_readyPending = false;
		_sendAvailable = true;
		return call(_readyCallback, HleReadyReturned, {});
	}
	
	if (_completedAcl)
	{
		std::vector<uint8_t> completed { 1 };
		appendLe16(completed, _connection);
		appendLe16(completed, std::exchange(_completedAcl, 0));
		queueEvent(0x13, completed);
	}
	
	if (!_events.empty())
	{
		const auto& event = _events.front();
		bleHleWrite(guest, _workspace + PacketOffset, event.data(), event.size());
		if (event[0] == 4 && (event[1] == 0x0E || event[1] == 0x0F)) _commandPending = false;
		if (event[0] == 4) std::fprintf(stderr, "BLE HLE: HCI event %02X bytes=%zu\n", event[1], event.size());
		_receiveInFlight = true;
		return call(_receiveCallback, HleReceiveReturned, { _workspace + PacketOffset, static_cast<uint32_t>(event.size()) });
	}
	
	_delayStartedNs = bleHleTimeNs();
	return call(_osi[OsiSemaphoreTake], HleDelayReturned, { _semaphore, _pollTimeoutMs });
}

static BleController controller;

} // namespace m5emulator::ble

extern "C" bool bleHleEnabled()
{
	static const bool Enabled = [] {
		const char* value = std::getenv("M5EMU_BLE_HLE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return Enabled;
}

extern "C" void bleHleReset()
{
	m5emulator::ble::controller = {};
	bleHleResetHooks();
}

extern "C" BleHleStep bleHleStep(void* guest, uint32_t operation, const uint32_t* registers)
{
	return m5emulator::ble::controller.step(guest, operation, std::span<const uint32_t, 16>(registers, 16));
}
