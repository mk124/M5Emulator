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
#include <chrono>
#include <iostream>
#include <utility>

namespace m5emulator::bluetooth {

static constexpr size_t MaxPendingRequests = 64;
static constexpr auto AttTimeout = std::chrono::seconds(30);

static void prepareWrite(AttRequest& request, uint16_t mtu, uint16_t offset)
{
	const uint16_t handle = readLe16(ByteView(request.packet).subspan(1));
	request.packet = { 0x16 };
	appendLe16(request.packet, handle);
	appendLe16(request.packet, offset);
	append(request.packet, ByteView(request.longValue).subspan(offset, std::min<size_t>(mtu - 5U, request.longValue.size() - offset)));
}

void VirtualPeer::exchangeMtu(uint16_t maximum, const AttRequest::Completion& completed)
{
	if (!_ready || maximum < DefaultAttMtu || maximum > MaxAttMtu)
	{
		completed(0x06, {});
		return;
	}
	
	if (_mtuExchanged)
	{
		completed(0, std::array<uint8_t, 2> { static_cast<uint8_t>(_mtu), static_cast<uint8_t>(_mtu >> 8) });
		return;
	}
	
	_localMtu = maximum;
	Bytes packet { 2 };
	appendLe16(packet, maximum);
	request(std::move(packet), [this, maximum, completed] (uint8_t error, ByteView value) {
		if (!error)
		{
			require(value.size() == 2 && readLe16(value) >= DefaultAttMtu, "invalid ATT MTU response");
			_mtu = std::min(_mtuExchanged ? _mtu : maximum, readLe16(value));
			_mtuExchanged = true;
		}
		completed(error, value);
	});
}

void VirtualPeer::read(uint16_t handle, uint16_t offset, const AttRequest::Completion& completed)
{
	if (!_ready)
	{
		completed(0x0E, {});
		return;
	}
	
	Bytes packet { static_cast<uint8_t>(offset ? 0x0C : 0x0A) };
	appendLe16(packet, handle);
	if (offset) appendLe16(packet, offset);
	request(std::move(packet), completed);
}

void VirtualPeer::write(uint16_t handle, ByteView value, const AttRequest::Completion& completed)
{
	if (!_ready || value.size() > MaxAttValueSize)
	{
		completed(_ready ? 0x0D : 0x0E, {});
		return;
	}
	
	Bytes packet { 0x12 };
	appendLe16(packet, handle);
	if (value.size() > _mtu - 3U)
	{
		// Preserve a physical GATT value as one guest write, even across different MTUs.
		request(std::move(packet), completed, Bytes(value.begin(), value.end()));
		return;
	}
	
	append(packet, value);
	request(std::move(packet), completed);
}

void VirtualPeer::request(Bytes&& packet, const AttRequest::Completion& completed, Bytes&& longValue)
{
	if (!_generation || _requests.size() >= MaxPendingRequests)
	{
		std::cerr << "BLE bridge: ATT request rejected; queued=" << _requests.size() << " connected=" << (_generation != 0) << '\n';
		completed(0x11, {});
		return;
	}
	
	AttRequest pending { std::move(packet), completed, std::move(longValue) };
	if (!pending.longValue.empty()) prepareWrite(pending, _mtu, 0);
	_requests.push_back(std::move(pending));
	sendNext();
}

void VirtualPeer::sendNext()
{
	if (_attBusy || _attSecurityPending || _requests.empty() || !_generation) return;
	
	_attBusy = true;
	_attDeadline = Clock::now() + AttTimeout;
	sendL2cap(4, _requests.front().packet);
}

void VirtualPeer::receiveAtt(ByteView packet)
{
	require(!packet.empty() && packet.size() <= _mtu, "invalid ATT PDU length");
	const uint8_t opcode = packet[0];
	if (opcode == 0x1B || opcode == 0x1D)
	{
		require(packet.size() >= 3, "truncated ATT notification");
		const uint16_t handle = readLe16(packet.subspan(1));
		// Restored CCCDs can produce values before discovery or physical subscription.
		if (_ready && onValue && !onValue(handle, packet.subspan(3), opcode == 0x1D))
		{
			disconnect();
			return;
		}
		if (opcode == 0x1D) sendL2cap(4, std::array<uint8_t, 1> { 0x1E });
		return;
	}
	
	if (opcode == 2)
	{
		require(packet.size() == 3 && readLe16(packet.subspan(1)) >= DefaultAttMtu, "invalid peer ATT MTU");
		// Both roles may initiate an exchange. Use the same receive limit in both directions.
		Bytes response { 3 };
		appendLe16(response, _localMtu);
		sendL2cap(4, response);
		_mtu = std::min(_localMtu, readLe16(packet.subspan(1)));
		_mtuExchanged = true;
		return;
	}
	
	if ((opcode & 1) == 0)
	{
		if (!(opcode & 0x40)) sendL2cap(4, std::array<uint8_t, 5> { 1, opcode, 0, 0, 6 });
		return;
	}
	
	require(_attBusy && !_requests.empty(), "unsolicited ATT response");
	const uint8_t requestOpcode = _requests.front().packet[0];
	uint8_t error = 0;
	if (opcode == 1)
	{
		require(packet.size() == 5 && packet[1] == requestOpcode && packet[4] != 0, "invalid ATT error response");
		error = packet[4];
		if ((error == 0x05 || error == 0x0F) && !_security.encrypted() && requestOpcode != 0x18)
		{
			_attBusy = false;
			_attSecurityPending = true;
			_security.start();
			return;
		}
	}
	else require(opcode == requestOpcode + 1, "ATT response opcode mismatch");
	
	AttRequest& active = _requests.front();
	if (requestOpcode == 0x16)
	{
		if (!error && !std::ranges::equal(packet.subspan(1), ByteView(active.packet).subspan(1))) error = 0x04;
		if (error)
		{
			active.prepareError = error;
			active.packet = { 0x18, 0 }; // Clear any fragments already prepared by NimBLE.
		}
		else
		{
			const size_t offset = readLe16(ByteView(active.packet).subspan(3)) + active.packet.size() - 5;
			if (offset < active.longValue.size()) prepareWrite(active, _mtu, static_cast<uint16_t>(offset));
			else active.packet = { 0x18, 1 };
		}
		
		// Keep the transaction at the front so later writes cannot interleave with it.
		_attBusy = false;
		sendNext();
		return;
	}
	if (requestOpcode == 0x18)
	{
		if (!error) require(packet.size() == 1, "invalid ATT execute-write response");
		if (active.prepareError)
		{
			require(!error, "ATT prepared-write cancellation failed");
			error = active.prepareError;
		}
	}
	
	auto completed = std::move(_requests.front().completed);
	_requests.pop_front();
	_attBusy = false;
	completed(error, error ? ByteView {} : packet.subspan(1));
	sendNext();
}

} // namespace m5emulator::bluetooth
