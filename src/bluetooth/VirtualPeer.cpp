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

#include "VirtualPeer.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "SmpCrypto.hpp"

namespace m5emulator::bluetooth {

static constexpr size_t MaxPacketsPerPoll = 64;
static constexpr size_t MaxL2capPayloadSize = 1020;
static constexpr auto DisconnectTimeout = std::chrono::seconds(10);
static constexpr auto SetupTimeout = std::chrono::seconds(90);

VirtualPeer::VirtualPeer(const std::filesystem::path& socketPath) : _socketPath(socketPath)
{
	const std::string path = socketPath.string();
	sockaddr_un address {};
	address.sun_family = AF_UNIX;
	require(path.size() < sizeof(address.sun_path), "BLE socket path is too long");
	std::strcpy(address.sun_path, path.c_str());
	
	_listener = socket(AF_UNIX, SOCK_STREAM, 0);
	if (_listener < 0) throw std::system_error(errno, std::generic_category(), "create BLE listener");
	if (fcntl(_listener, F_SETFL, O_NONBLOCK) < 0 || fcntl(_listener, F_SETFD, FD_CLOEXEC) < 0 ||
	    bind(_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(_listener, 1) < 0)
	{
		const int error = errno;
		close(_listener);
		throw std::system_error(error, std::generic_category(), "listen for QEMU BLE");
	}
	
	randomBytes(std::span(_localAddress).first(6));
	_localAddress[5] |= 0xC0;
	_localAddress[6] = 1;
}

VirtualPeer::~VirtualPeer()
{
	close(_listener);
	std::error_code ignored;
	std::filesystem::remove(_socketPath, ignored);
}

void VirtualPeer::poll()
{
	try
	{
		if (!_socket.connected())
		{
			if (_transport)
			{
				_transport = false;
				_advertising = _closing = false;
				_closingGeneration = 0;
				clearLink();
				invalidateCatalog();
			}
			
			const int fd = accept(_listener, nullptr, nullptr);
			if (fd >= 0)
			{
				_socket.adopt(fd);
				_transport = true;
				_failed = false;
				std::cout << "BLE bridge: QEMU transport connected\n"
				          << std::flush;
			}
			else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			{
				throw std::system_error(errno, std::generic_category(), "accept QEMU BLE");
			}
		}
		
		_socket.flush();
		for (size_t count = 0; count < MaxPacketsPerPoll; ++count)
		{
			const Bytes packet = _socket.receive();
			if (packet.empty()) break;
			receive(packet);
		}
		
		if (_enabled && !_failed && _advertising && !_generation && !_connecting && !_closing && _sessionRequested) connect();
		if (_attBusy || _attSecurityPending) require(Clock::now() < _attDeadline, "ATT transaction timed out");
		if ((_connecting || _generation || _closing) && !_ready) require(Clock::now() < _setupDeadline, "virtual BLE setup timed out");
	}
	catch (const std::exception& error)
	{
		std::cerr << "BLE bridge: " << error.what() << "; session closed\n";
		_failed = true;
		if (_closing && Clock::now() >= _setupDeadline)
		{
			_socket.close();
			_closing = false;
			_closingGeneration = 0;
		}
		disconnect();
		invalidateCatalog();
	}
}

void VirtualPeer::setEnabled(bool enabled)
{
	if (enabled == _enabled) return;
	
	_enabled = enabled;
	if (enabled) _failed = false;
	else disconnect();
	
	if (onAvailabilityChanged) onAvailabilityChanged();
}

void VirtualPeer::reset()
{
	_socket.close();
	_transport = false;
	_advertising = false;
	_failed = false;
	_closing = false;
	_closingGeneration = 0;
	clearLink();
	invalidateCatalog();
}

void VirtualPeer::disconnect()
{
	if (_closing) return;
	
	_closing = _generation || _connecting;
	_closingGeneration = _generation;
	if (_generation) _socket.send(ble::PeerMessage::Disconnect, _generation, std::array<uint8_t, 1> { 0x13 });
	_setupDeadline = Clock::now() + DisconnectTimeout;
	clearLink();
}

bool VirtualPeer::startSession()
{
	if (!available()) return false;
	
	_sessionRequested = true;
	return true;
}

void VirtualPeer::clearLink()
{
	const bool active = _generation || _connecting || _ready || _sessionRequested;
	_generation = 0;
	_connecting = _ready = _sessionRequested = false;
	_mtu = _localMtu = DefaultAttMtu;
	_mtuExchanged = _attBusy = _attSecurityPending = false;
	_discoveredServices.clear();
	_security.reset();
	
	auto pending = std::move(_requests);
	_requests.clear();
	for (auto& request : pending) request.completed(0x0E, {});
	if (active && onClosed) onClosed();
}

void VirtualPeer::receive(ByteView packet)
{
	require(packet.size() >= 9, "truncated virtual-link message");
	const auto kind = static_cast<ble::PeerMessage>(packet[0]);
	uint64_t generation = 0;
	for (size_t i = 0; i < 8; ++i) generation |= static_cast<uint64_t>(packet[i + 1]) << (i * 8);
	const ByteView payload = packet.subspan(9);
	
	if (kind == ble::PeerMessage::GattDatabase)
	{
		receiveCatalog(payload);
		return;
	}
	
	if (kind == ble::PeerMessage::Reset)
	{
		_failed = false;
		_advertising = _closing = false;
		_closingGeneration = 0;
		clearLink();
		invalidateCatalog();
		return;
	}
	
	if (kind == ble::PeerMessage::Disconnected && _closing && generation == _closingGeneration)
	{
		_closing = false;
		_closingGeneration = 0;
		return;
	}
	
	if (kind == ble::PeerMessage::Advertising)
	{
		advertisement(payload);
		return;
	}
	
	if (kind == ble::PeerMessage::Connected)
	{
		require(generation && payload.size() == 13, "invalid virtual connection");
		if (!_connecting || !_enabled)
		{
			_closing = true;
			_closingGeneration = generation;
			_socket.send(ble::PeerMessage::Disconnect, generation, std::array<uint8_t, 1> { 0x13 });
			return;
		}
		
		_generation = generation;
		_connecting = false;
		_advertising = false;
		if (_security.bonded())
		{
			// Restore encryption before ATT can change CCCDs which NimBLE will reload from NVS.
			_attSecurityPending = true;
			_attDeadline = _setupDeadline;
			_security.start();
		}
		discoverServices(1);
		return;
	}
	
	if (kind == ble::PeerMessage::Rejected)
	{
		// A terminated generation may still have already queued packets in the transport.
		if (generation && generation != _generation) return;
		require(false, "QEMU rejected a virtual BLE operation");
	}
	
	if (generation != _generation || !_generation) return;
	
	switch (kind)
	{
		case ble::PeerMessage::Disconnected: {
			clearLink();
			break;
		}
		case ble::PeerMessage::Data: {
			receiveL2cap(payload);
			break;
		}
		case ble::PeerMessage::Encryption: {
			require(payload.size() == 1, "invalid virtual encryption result");
			_security.encryptionChanged(payload[0]);
			break;
		}
		default: throw std::runtime_error("unknown virtual-link message");
	}
}

void VirtualPeer::advertisement(ByteView payload)
{
	require(payload.size() >= 10, "truncated virtual advertisement");
	_remoteAddress = fixedBytes<7>(payload.subspan(1, 7));
	std::rotate(_remoteAddress.begin(), _remoteAddress.begin() + 1, _remoteAddress.end());
	_advertising = payload[0] != 0;
	
	const ByteView data = payload.subspan(1);
	const bool changed = !std::ranges::equal(data, _advertisementData);
	_advertisementData.assign(data.begin(), data.end());
	if ((_catalogReady && changed) || (!_advertising && !_generation && !_connecting && !_closing))
	{
		disconnect();
		invalidateCatalog();
	}
	
	_name.clear();
	_advertisedServices.clear();
	for (size_t offset = 8; offset < payload.size();)
	{
		const size_t size = payload[offset++];
		require(offset + size <= payload.size(), "truncated advertising data");
		const ByteView fields = payload.subspan(offset, size);
		offset += size;
		for (size_t position = 0; position < fields.size();)
		{
			const size_t length = fields[position++];
			if (!length) break;
			require(position + length <= fields.size(), "invalid advertising field");
			const uint8_t type = fields[position];
			const ByteView value = fields.subspan(position + 1, length - 1);
			if (type == 8 || type == 9) _name.assign(reinterpret_cast<const char*>(value.data()), value.size());
			if (type == 2 || type == 3 || type == 6 || type == 7)
			{
				const size_t uuidSize = type <= 3 ? 2 : 16;
				require(value.size() % uuidSize == 0, "invalid advertised UUID list");
				for (size_t i = 0; i < value.size(); i += uuidSize) _advertisedServices.push_back(uuidString(value.subspan(i, uuidSize)));
			}
			position += length;
		}
	}
	
	if (onAvailabilityChanged) onAvailabilityChanged();
}

void VirtualPeer::connect()
{
	Bytes request { _localAddress[6] };
	append(request, ByteView(_localAddress).first(6));
	appendLe16(request, 24);
	appendLe16(request, 0);
	appendLe16(request, 200);
	
	_discoveredServices.clear();
	_connecting = true;
	_setupDeadline = Clock::now() + SetupTimeout;
	_socket.send(ble::PeerMessage::Connect, 0, request);
}

void VirtualPeer::sendL2cap(uint16_t channel, ByteView payload)
{
	require(_generation != 0 && payload.size() <= MaxL2capPayloadSize, "invalid outbound L2CAP packet");
	
	Bytes packet;
	appendLe16(packet, payload.size());
	appendLe16(packet, channel);
	append(packet, payload);
	_socket.send(ble::PeerMessage::L2cap, _generation, packet);
}

void VirtualPeer::receiveL2cap(ByteView payload)
{
	require(payload.size() >= 4 && readLe16(payload) == payload.size() - 4, "invalid inbound L2CAP packet");
	const uint16_t channel = readLe16(payload.subspan(2));
	const ByteView packet = payload.subspan(4);
	if (channel == 4) receiveAtt(packet);
	else if (channel == 6) _security.receive(packet);
	else if (channel == 5)
	{
		require(packet.size() >= 4 && readLe16(packet.subspan(2)) == packet.size() - 4, "invalid LE signaling packet");
		// Connection parameter changes are handled by HCI; reject unsupported signaling requests.
		if (packet[0] != 1) sendL2cap(5, std::array<uint8_t, 6> { 1, packet[1], 2, 0, 0, 0 });
	}
	else throw std::runtime_error("unsupported virtual L2CAP channel");
}

void VirtualPeer::requestEncryption(const Key128& key)
{
	Bytes request(10, 0); // SC uses zero Rand and EDIV.
	append(request, key);
	_socket.send(ble::PeerMessage::Encrypt, _generation, request);
}

void VirtualPeer::securityReady()
{
	_attSecurityPending = false;
	sendNext();
}

} // namespace m5emulator::bluetooth
