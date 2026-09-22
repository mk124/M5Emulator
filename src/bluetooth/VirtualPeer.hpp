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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "PeerSocket.hpp"

#include "AttRequest.hpp"
#include "BleBytes.hpp"
#include "GattService.hpp"
#include "SmpSession.hpp"

namespace m5emulator::bluetooth {

class VirtualPeer
{
	friend class SmpSession;
	
	using Clock = std::chrono::steady_clock;
	
	static constexpr size_t MaxServices = 32;
	static constexpr size_t MaxCharacteristics = 64;
	static constexpr size_t MaxDescriptors = 32;
	
public:
	explicit VirtualPeer(const std::filesystem::path& socketPath);
	~VirtualPeer();
	
	VirtualPeer(const VirtualPeer&) = delete;
	VirtualPeer& operator=(const VirtualPeer&) = delete;
	
	void poll();
	void setEnabled(bool enabled);
	void reset();
	void disconnect();
	bool startSession();
	
	void exchangeMtu(uint16_t maximum, const AttRequest::Completion& completed);
	void read(uint16_t handle, uint16_t offset, const AttRequest::Completion& completed);
	void write(uint16_t handle, ByteView value, const AttRequest::Completion& completed);
	
private:
	void clearLink();
	void receive(ByteView packet);
	void advertisement(ByteView payload);
	void connect();
	void sendL2cap(uint16_t channel, ByteView payload);
	void receiveL2cap(ByteView payload);
	void requestEncryption(const Key128& key);
	void securityReady();
	
	void request(Bytes&& packet, const AttRequest::Completion& completed, Bytes&& longValue = {});
	void sendNext();
	void receiveAtt(ByteView packet);
	
	void invalidateCatalog();
	void receiveCatalog(ByteView payload);
	
	void discoverServices(uint16_t start);
	void discoverCharacteristics(size_t service, uint16_t start);
	void discoverDescriptors(size_t service, size_t characteristic, uint16_t start);
	void discoveryComplete();
	void clearSubscriptions(size_t service, size_t characteristic);
	
public:
	bool available() const { return _catalogReady && _enabled && !_failed && !_closing && (_advertising || _generation || _connecting || _sessionRequested); }
	bool ready() const { return _ready; }
	uint16_t mtu() const { return _mtu; }
	uint64_t generation() const { return _generation; }
	const std::vector<GattService>& services() const { return _services; }
	const std::string& name() const { return _name; }
	const std::vector<std::string>& advertisedServices() const { return _advertisedServices; }
	const Address& localAddress() const { return _localAddress; }
	const Address& remoteAddress() const { return _remoteAddress; }
	
	std::function<void()> onReady, onClosed, onAvailabilityChanged;
	std::function<bool(uint16_t, ByteView, bool)> onValue;
	
private:
	std::filesystem::path _socketPath;
	int _listener = -1;
	ble::PeerSocket _socket;
	bool _transport = false, _enabled = true, _failed = false;
	
	Address _localAddress {}, _remoteAddress {};
	bool _advertising = false;
	std::string _name;
	std::vector<std::string> _advertisedServices;
	Bytes _advertisementData;
	
	Bytes _catalogBuffer;
	std::vector<GattService> _services, _discoveredServices;
	bool _catalogReady = false, _catalogReceiving = false;
	
	bool _connecting = false, _closing = false, _ready = false, _sessionRequested = false;
	uint64_t _generation = 0, _closingGeneration = 0;
	Clock::time_point _setupDeadline {};
	SmpSession _security { *this };
	
	uint16_t _mtu = DefaultAttMtu, _localMtu = DefaultAttMtu;
	bool _mtuExchanged = false, _attBusy = false, _attSecurityPending = false;
	Clock::time_point _attDeadline {};
	std::deque<AttRequest> _requests;
};

} // namespace m5emulator::bluetooth
