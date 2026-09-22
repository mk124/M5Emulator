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
#include <iterator>
#include <utility>

#include "BleBytes.hpp"

namespace m5emulator::ble {

static constexpr size_t ResolvingCapacity = 4;

void BleController::resetHci()
{
	if (_connection) disconnect(0x16);
	_eventMask = {};
	_leEventMask = {};
	_eventMaskPage2 = {};
	_randomAddress = {};
	_advertisingParameters = {};
	_advertisingData.clear();
	_scanResponse.clear();
	_advertising = false;
	_resolutionEnabled = false;
	_rpaTimeout = 900;
	_resolvingList.clear();
	_privacyModes = {};
	
	_peer.send(PeerMessage::Reset, 0, {});
}

void BleController::queueEvent(uint8_t code, std::span<const uint8_t> parameters)
{
	if (code != 0x0E && code != 0x0F && code != 0x13)
	{
		const unsigned bit = code - 1;
		if (bit >= 64 || !(_eventMask[bit / 8] & (1u << (bit % 8)))) return;
	}
	
	if (parameters.size() > 68 || _events.size() >= 64) fail("HCI event capacity exceeded");
	std::vector<uint8_t> event { 4, code, static_cast<uint8_t>(parameters.size()) };
	event.insert(event.end(), parameters.begin(), parameters.end());
	_events.push_back(std::move(event));
}

void BleController::queueLeEvent(uint8_t subevent, std::span<const uint8_t> parameters)
{
	const unsigned bit = subevent - 1;
	if (bit >= 64 || !(_leEventMask[bit / 8] & (1u << (bit % 8)))) return;
	
	std::vector<uint8_t> payload { subevent };
	payload.insert(payload.end(), parameters.begin(), parameters.end());
	queueEvent(0x3E, payload);
}

void BleController::completeCommand(uint16_t opcode, std::span<const uint8_t> result, size_t eventIndex)
{
	const bool asynchronous = opcode == 0x0406 || opcode == 0x041D || opcode == 0x2013 || opcode == 0x2016 || opcode == 0x2032;
	std::vector<uint8_t> event;
	if (asynchronous) event = { 4, 0x0F, 4, result[0], 1 };
	else event = { 4, 0x0E, static_cast<uint8_t>(3 + result.size()), 1 };
	appendLe16(event, opcode);
	if (!asynchronous) event.insert(event.end(), result.begin(), result.end());
	_events.insert(_events.begin() + eventIndex, std::move(event));
}

void BleController::send(void* guest, uint32_t address, uint32_t length)
{
	if (length < 4 || length > 259) fail("invalid H4 packet length");
	std::vector<uint8_t> bytes(length);
	bleHleRead(guest, address, bytes.data(), bytes.size());
	if (bytes[0] == 2) sendAcl(std::span(bytes).subspan(1));
	else if (bytes[0] == 1)
	{
		if (bytes[3] + 4u != length) fail("inconsistent HCI parameter length");
		if (_commandPending) fail("HCI command credit exceeded");
		
		++_commands;
		_commandPending = true;
		const uint16_t opcode = readLe16(std::span(bytes).subspan(1));
		std::fprintf(stderr, "BLE HLE: HCI command %04X bytes=%u\n", opcode, length);
		
		const size_t eventIndex = _events.size();
		const auto reply = command(opcode, std::span(bytes).subspan(4));
		completeCommand(opcode, reply, eventIndex);
		if (opcode == 0x200A && reply[0] == 0 && _advertising) publishGatt(guest);
	}
	else fail("unsupported H4 packet type");
	
	_sendAvailable = false;
	_readyPending = true;
}

std::vector<uint8_t> BleController::command(uint16_t opcode, std::span<const uint8_t> parameters)
{
	if (auto result = connectionCommand(opcode, parameters)) return std::move(*result);
	
	static constexpr std::pair<uint16_t, uint8_t> CommandLengths[] {
		{ 0x0C03, 0 },
		{ 0x1001, 0 },
		{ 0x1002, 0 },
		{ 0x1003, 0 },
		{ 0x1009, 0 },
		{ 0x0C01, 8 },
		{ 0x0C63, 8 },
		{ 0x2001, 8 },
		{ 0x2002, 0 },
		{ 0x2003, 0 },
		{ 0x2005, 6 },
		{ 0x2006, 15 },
		{ 0x2007, 0 },
		{ 0x2008, 32 },
		{ 0x2009, 32 },
		{ 0x200A, 1 },
		{ 0x2018, 0 },
		{ 0x2027, 39 },
		{ 0x2028, 7 },
		{ 0x2029, 0 },
		{ 0x202A, 0 },
		{ 0x202D, 1 },
		{ 0x202E, 2 },
		{ 0x204E, 8 }
	};
	const auto entry = std::ranges::find(CommandLengths, opcode, &std::pair<uint16_t, uint8_t>::first);
	if (entry == std::end(CommandLengths))
	{
		std::fprintf(stderr, "BLE HLE: unsupported HCI opcode %04X\n", opcode);
		return { 1 };
	}
	if (parameters.size() != entry->second) return { 0x12 };
	
	switch (opcode)
	{
		case 0x0C03: {
			resetHci();
			break;
		}
		case 0x1001:
			return { 0, 9, 0, 0, 9, 0xFF, 0xFF, 0, 0 };
		case 0x1002: {
			std::vector<uint8_t> reply(65, 0);
			static constexpr unsigned SupportedCommandBits[] {
				0 * 8 + 5, 2 * 8 + 7, 15 * 8 + 5,
				27 * 8 + 2, 27 * 8 + 5, 28 * 8 + 1, 28 * 8 + 2, 33 * 8 + 6, 33 * 8 + 7, 35 * 8 + 3,
				35 * 8 + 4, 35 * 8 + 6,
				5 * 8 + 6, 5 * 8 + 7, 14 * 8 + 3, 14 * 8 + 5, 15 * 8 + 1, 22 * 8 + 2,
				25 * 8, 25 * 8 + 1, 25 * 8 + 2, 25 * 8 + 4, 25 * 8 + 5, 25 * 8 + 6, 25 * 8 + 7,
				26 * 8, 26 * 8 + 1, 27 * 8 + 7, 34 * 8 + 3, 34 * 8 + 4, 34 * 8 + 5, 34 * 8 + 6,
				35 * 8 + 1, 35 * 8 + 2, 39 * 8 + 2
			};
			for (const unsigned bit : SupportedCommandBits) reply[1 + bit / 8] |= 1u << (bit % 8);
			return reply;
		}
		case 0x1003:
			return { 0, 0, 0, 0, 0, 0x60, 0, 0, 0 };
		case 0x1009:
			return { 0, 0x56, 0x34, 0x12, 0x38, 0xC1, 0x02 };
		case 0x2002:
			return { 0, 0xFB, 0, 12 };
		case 0x2003:
			return { 0, 0x63, 0x01, 0, 0, 0, 0, 0, 0 }; // Encryption, DLE, privacy, 2M.
		case 0x0C01: {
			std::ranges::copy(parameters, _eventMask.begin());
			break;
		}
		case 0x0C63: {
			std::ranges::copy(parameters, _eventMaskPage2.begin());
			break;
		}
		case 0x2001: {
			std::ranges::copy(parameters, _leEventMask.begin());
			break;
		}
		case 0x2005: {
			if (_advertising) return { 0x0C };
			std::ranges::copy(parameters, _randomAddress.begin());
			break;
		}
		case 0x2006: {
			if (_advertising) return { 0x0C };
			if (parameters[4] > 4 || parameters[5] > 3 || parameters[6] > 1 || parameters[13] == 0 || parameters[13] > 7 || parameters[14] > 3) return { 0x12 };
			if (readLe16(parameters) < 0x20 || readLe16(parameters.subspan(2)) > 0x4000 || readLe16(parameters) > readLe16(parameters.subspan(2))) return { 0x12 };
			std::ranges::copy(parameters, _advertisingParameters.begin());
			break;
		}
		case 0x2007: {
			const uint8_t level = _advertisingPower.value_or(_defaultPower);
			return { 0, static_cast<uint8_t>(level == 15 ? 20 : -24 + 3 * level) };
		}
		case 0x2008:
		case 0x2009: {
			if (parameters[0] > 31) return { 0x12 };
			auto& destination = opcode == 0x2008 ? _advertisingData : _scanResponse;
			destination.assign(parameters.begin() + 1, parameters.begin() + 1 + parameters[0]);
			break;
		}
		case 0x200A: {
			if (parameters[0] > 1) return { 0x12 };
			if (_connection && parameters[0]) return { 0x0C };
			if (parameters[0] && (_advertisingParameters[5] & 1) && std::ranges::all_of(_randomAddress, [] (uint8_t byte) { return byte == 0; })) return { 0x12 };
			_advertising = parameters[0];
			advertiseToPeer();
			std::fprintf(stderr, "BLE HLE: ADVERTISING %s commands=%u received=%u ready=%u delay-resumes=%u\n", _advertising ? "ON" : "OFF", _commands, _received, _ready, _delayResumes);
			tracePacket("advertisement", _advertisingData);
			tracePacket("scan-response", _scanResponse);
			break;
		}
		case 0x2018: {
			std::vector<uint8_t> reply(9, 0);
			arc4random_buf(reply.data() + 1, 8);
			return reply;
		}
		case 0x2027: {
			if (_resolutionEnabled && _advertising) return { 0x0C };
			if (parameters[0] > 1) return { 0x12 };
			const auto sameIdentity = [&] (const auto& item) { return std::equal(parameters.begin(), parameters.begin() + 7, item.begin()); };
			if (std::ranges::any_of(_resolvingList, sameIdentity)) return { 0x12 };
			if (_resolvingList.size() == ResolvingCapacity) return { 7 };
			
			std::array<uint8_t, 39> item {};
			std::ranges::copy(parameters, item.begin());
			_resolvingList.push_back(item);
			break;
		}
		case 0x2028: {
			if (_resolutionEnabled && _advertising) return { 0x0C };
			const auto entry = std::ranges::find_if(_resolvingList, [&] (const auto& item) { return std::equal(parameters.begin(), parameters.end(), item.begin()); });
			if (entry == _resolvingList.end()) return { 2 };
			
			const size_t index = entry - _resolvingList.begin();
			for (size_t i = index; i + 1 < _resolvingList.size(); ++i) _privacyModes[i] = _privacyModes[i + 1];
			_privacyModes[_resolvingList.size() - 1] = 0;
			_resolvingList.erase(entry);
			break;
		}
		case 0x2029: {
			if (_resolutionEnabled && _advertising) return { 0x0C };
			_resolvingList.clear();
			_privacyModes = {};
			break;
		}
		case 0x202A:
			return { 0, ResolvingCapacity };
		case 0x202D: {
			if (_advertising) return { 0x0C };
			if (parameters[0] > 1) return { 0x12 };
			_resolutionEnabled = parameters[0];
			break;
		}
		case 0x202E: {
			if (readLe16(parameters) == 0 || readLe16(parameters) > 0xA1B8) return { 0x12 };
			_rpaTimeout = readLe16(parameters);
			break;
		}
		case 0x204E: {
			if (parameters[0] > 1 || parameters[7] > 1) return { 0x12 };
			const auto entry = std::ranges::find_if(_resolvingList, [&] (const auto& item) { return std::equal(parameters.begin(), parameters.begin() + 7, item.begin()); });
			if (entry == _resolvingList.end()) return { 2 };
			_privacyModes[entry - _resolvingList.begin()] = parameters[7];
			break;
		}
	}
	return { 0 };
}

} // namespace m5emulator::ble
