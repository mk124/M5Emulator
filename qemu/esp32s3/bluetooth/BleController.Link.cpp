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
#include <utility>

#include "BleBytes.hpp"

namespace m5emulator::ble {

// F5 starts a new QEMU process; its first connection must also have a new identity.
static uint64_t nextGeneration = [] {
	uint64_t generation;
	arc4random_buf(&generation, sizeof(generation));
	return generation & 0x7FFFFFFFFFFFFFFF;
}();
static uint16_t nextConnectionHandle = 0;

static constexpr size_t MaxL2capBytes = 1024, AclPayloadBytes = 251;

void BleController::pollPeer()
{
	_peer.open();
	_peer.flush();
	while (_events.size() < 24)
	{
		const auto packet = _peer.receive();
		if (packet.empty()) break;
		peerMessage(packet);
	}
	
	if (_connection && !_peer.connected()) disconnect(0x08);
}

void BleController::peerMessage(std::span<const uint8_t> packet)
{
	const auto message = static_cast<PeerMessage>(packet[0]);
	uint64_t generation = 0;
	for (unsigned i = 0; i < 8; ++i) generation |= static_cast<uint64_t>(packet[1 + i]) << (8 * i);
	const auto payload = packet.subspan(9);
	
	const auto reject = [&] (uint8_t status) { _peer.send(PeerMessage::Rejected, generation, std::array { packet[0], status }); };
	if (message != PeerMessage::Connect && (!_connection || generation != _generation))
	{
		reject(0x02);
		return;
	}
	
	switch (message)
	{
		case PeerMessage::Connect: {
			if (generation != 0 || _connection || !_advertising || _status != 2 || _advertisingParameters[4] != 0) return reject(0x0C);
			if (payload.size() != 13 || payload[0] > 1) return reject(0x12);
			if (payload[0] == 1 && (payload[6] & 0xC0) != 0xC0) return reject(0x11); // Static virtual identities only.
			const uint16_t interval = readLe16(payload.subspan(7)), latency = readLe16(payload.subspan(9)), timeout = readLe16(payload.subspan(11));
			if (interval < 6 || interval > 3200 || latency > 499 || timeout < 10 || timeout > 3200 || timeout * 4u <= (latency + 1u) * interval) return reject(0x12);
			
			_interval = interval;
			_latency = latency;
			_supervisionTimeout = timeout;
			_generation = ++nextGeneration;
			_connection = nextConnectionHandle = nextConnectionHandle == 0xEFF ? 1 : nextConnectionHandle + 1;
			_advertising = false;
			_phy = 1;
			_encrypted = false;
			_aclAssembly.clear();
			_pendingKey.reset();
			
			std::vector<uint8_t> complete { 0 };
			appendLe16(complete, _connection);
			complete.push_back(1); // Original firmware is the peripheral.
			complete.insert(complete.end(), payload.begin(), payload.begin() + 7);
			const bool enhanced = (_leEventMask[1] & 2) != 0;
			if (enhanced) complete.insert(complete.end(), 12, 0); // Identity addresses; no RPA used on this link.
			complete.insert(complete.end(), payload.begin() + 7, payload.end());
			complete.push_back(0);
			queueLeEvent(enhanced ? 0x0A : 0x01, complete);
			_peer.send(PeerMessage::Connected, _generation, payload);
			std::fprintf(stderr, "BLE HLE: CONNECTED generation=%llu handle=%u\n", static_cast<unsigned long long>(_generation), _connection);
			break;
		}
		case PeerMessage::Disconnect: {
			if (payload.size() != 1) return reject(0x12);
			disconnect(payload[0]);
			break;
		}
		case PeerMessage::L2cap: {
			if (payload.size() < 4 || payload.size() > MaxL2capBytes || readLe16(payload) + 4u != payload.size()) return reject(0x12);
			receiveL2cap(payload);
			break;
		}
		case PeerMessage::Encrypt: {
			if (payload.size() != 26) return reject(0x12);
			if (_pendingKey || _encrypted) return reject(0x0C);
			
			_pendingKey.emplace();
			std::ranges::copy(payload.subspan(10), _pendingKey->begin());
			
			std::vector<uint8_t> request;
			appendLe16(request, _connection);
			request.insert(request.end(), payload.begin(), payload.begin() + 10);
			queueLeEvent(0x05, request);
			break;
		}
		default:
			reject(0x01);
	}
}

void BleController::advertiseToPeer()
{
	// The software link uses identity addresses, including the fallback for own types 2/3.
	const uint8_t type = _advertisingParameters[5] & 1;
	std::vector<uint8_t> payload { static_cast<uint8_t>(_advertising), type };
	if (type) payload.insert(payload.end(), _randomAddress.begin(), _randomAddress.end());
	else payload.insert(payload.end(), { 0x56, 0x34, 0x12, 0x38, 0xC1, 0x02 });
	payload.push_back(_advertisingData.size());
	payload.insert(payload.end(), _advertisingData.begin(), _advertisingData.end());
	payload.push_back(_scanResponse.size());
	payload.insert(payload.end(), _scanResponse.begin(), _scanResponse.end());
	_peer.send(PeerMessage::Advertising, 0, payload);
}

void BleController::disconnect(uint8_t reason)
{
	if (!_connection) return;
	std::vector<uint8_t> event { 0 };
	appendLe16(event, _connection);
	event.push_back(reason);
	queueEvent(0x05, event);
	_peer.send(PeerMessage::Disconnected, _generation, std::array { reason });
	std::fprintf(stderr, "BLE HLE: DISCONNECTED generation=%llu reason=%02X ACL sent=%u received=%u continuations=%u\n", static_cast<unsigned long long>(_generation), reason, _aclSent, _aclReceived, _aclFragments);
	
	_connection = 0;
	_encrypted = false;
	_pendingKey.reset();
	_aclAssembly.clear();
	_completedAcl = 0; // HCI disconnection releases all remaining packets for this handle.
}

void BleController::receiveL2cap(std::span<const uint8_t> packet)
{
	for (size_t offset = 0; offset < packet.size(); offset += AclPayloadBytes)
	{
		const auto fragment = packet.subspan(offset, std::min(AclPayloadBytes, packet.size() - offset));
		std::vector<uint8_t> acl { 2 };
		appendLe16(acl, _connection | (offset == 0 ? 0x2000 : 0x1000));
		appendLe16(acl, fragment.size());
		acl.insert(acl.end(), fragment.begin(), fragment.end());
		_events.push_back(std::move(acl));
		++_aclReceived;
		if (offset) ++_aclFragments;
	}
}

void BleController::sendAcl(std::span<const uint8_t> packet)
{
	if (packet.size() < 4 || packet.size() > AclPayloadBytes + 4 || readLe16(packet.subspan(2)) + 4u != packet.size()) fail("invalid outbound ACL length");
	const uint16_t header = readLe16(packet), handle = header & 0x0FFF;
	const unsigned boundary = header >> 12 & 3;
	if ((header & 0xC000) || boundary == 3) fail("invalid ACL flags");
	// The host may still reply to queued data before it receives Disconnection Complete.
	// That event releases the old handle's credits; late packets must not affect a new link.
	if (!_connection || handle != _connection) return;
	
	if (boundary == 1)
	{
		if (_aclAssembly.empty()) fail("ACL continuation without an SDU");
		++_aclFragments;
	}
	else if (!_aclAssembly.empty()) fail("new ACL SDU before the previous one completed");
	_aclAssembly.insert(_aclAssembly.end(), packet.begin() + 4, packet.end());
	if (_aclAssembly.size() < 4) fail("ACL start did not contain an L2CAP header");
	const size_t expectedSize = readLe16(_aclAssembly) + 4;
	if (expectedSize > MaxL2capBytes || _aclAssembly.size() > expectedSize) fail("invalid L2CAP SDU length");
	if (_aclAssembly.size() == expectedSize)
	{
		_peer.send(PeerMessage::Data, _generation, _aclAssembly);
		_aclAssembly.clear();
	}
	
	if (++_completedAcl > 12) fail("ACL controller credits exceeded");
	++_aclSent;
}

void BleController::finishEncryption(uint8_t status)
{
	_pendingKey.reset();
	_encrypted = status == 0;
	
	std::vector<uint8_t> event { status };
	appendLe16(event, _connection);
	event.push_back(_encrypted);
	queueEvent(0x08, event);
	_peer.send(PeerMessage::Encryption, _generation, std::array { status });
	std::fprintf(stderr, "BLE HLE: encryption %s after LTK validation\n", _encrypted ? "enabled" : "rejected");
}

} // namespace m5emulator::ble
