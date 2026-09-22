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

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "BleHle.h"
#include "PeerSocket.hpp"

namespace m5emulator::ble {

class BleController
{
public:
	BleHleStep step(void* guest, uint32_t operation, std::span<const uint32_t, 16> registers);
	
private:
	BleHleStep poll(void* guest);
	
	void resetHci();
	void queueEvent(uint8_t code, std::span<const uint8_t> parameters);
	void queueLeEvent(uint8_t subevent, std::span<const uint8_t> parameters);
	void completeCommand(uint16_t opcode, std::span<const uint8_t> result, size_t eventIndex);
	void send(void* guest, uint32_t address, uint32_t length);
	std::vector<uint8_t> command(uint16_t opcode, std::span<const uint8_t> parameters);
	
	std::optional<std::vector<uint8_t>> connectionCommand(uint16_t opcode, std::span<const uint8_t> parameters);
	
	void pollPeer();
	void peerMessage(std::span<const uint8_t> packet);
	void advertiseToPeer();
	void disconnect(uint8_t reason);
	
	void receiveL2cap(std::span<const uint8_t> packet);
	void sendAcl(std::span<const uint8_t> packet);
	
	void finishEncryption(uint8_t status);
	
	void publishGatt(void* guest);
	
	// Guest controller lifecycle and OSI callbacks.
	std::array<uint32_t, 32> _osi {};
	uint32_t _status = 0, _workspace = 0, _task = 0, _semaphore = 0;
	uint32_t _workerStackBytes = 0, _workerPriority = 0, _workerCore = 0, _pollTimeoutMs = 10;
	bool _initFailed = false;
	
	// Radio power and controller features.
	uint8_t _sleepMode = 0;
	bool _sleepEnabled = false, _wakeupRequested = false, _phyEnabled = false;
	std::array<uint32_t, 8> _offloadCallbacks {};
	std::array<bool, 5> _features {};
	
	// VHCI delivery and worker polling.
	uint32_t _receiveCallback = 0, _readyCallback = 0;
	uint32_t _commands = 0, _received = 0, _ready = 0, _delayResumes = 0;
	uint64_t _delayStartedNs = 0;
	uint8_t _shortWaits = 0;
	bool _sendAvailable = true, _readyPending = false, _commandPending = false;
	bool _receiveInFlight = false, _receiveCancelled = false;
	std::deque<std::vector<uint8_t>> _events;
	
	// Peer connection and encryption.
	PeerSocket _peer;
	uint64_t _generation = 0;
	uint16_t _connection = 0, _interval = 24, _latency = 0, _supervisionTimeout = 200;
	uint8_t _phy = 1, _connectionPower = 9;
	bool _encrypted = false;
	std::optional<std::array<uint8_t, 16>> _pendingKey;
	
	// ACL reassembly and flow control.
	std::vector<uint8_t> _aclAssembly;
	uint16_t _completedAcl = 0;
	uint32_t _aclSent = 0, _aclReceived = 0, _aclFragments = 0;
	
	// HCI event masks, advertising and address resolution.
	std::array<uint8_t, 8> _eventMask {}, _leEventMask {}, _eventMaskPage2 {};
	std::array<uint8_t, 6> _randomAddress {};
	std::array<uint8_t, 15> _advertisingParameters {};
	std::vector<uint8_t> _advertisingData, _scanResponse;
	bool _advertising = false, _resolutionEnabled = false;
	uint16_t _rpaTimeout = 900;
	std::vector<std::array<uint8_t, 39>> _resolvingList;
	std::array<uint8_t, 4> _privacyModes {};
	
	uint8_t _defaultPower = 9;
	std::optional<uint8_t> _advertisingPower;
};

} // namespace m5emulator::ble
