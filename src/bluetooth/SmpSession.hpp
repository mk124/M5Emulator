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
#include <cstdint>
#include <optional>

#include "BleBytes.hpp"
#include "SmpKeyPair.hpp"

namespace m5emulator::bluetooth {

class VirtualPeer;

class SmpSession
{
	enum class State
	{
		Idle,
		Response,
		PublicKey,
		Confirm,
		Random,
		Check,
		Encrypting,
		Restoring,
		Encrypted
	};
	
public:
	explicit SmpSession(VirtualPeer& peer);
	
	void reset();
	void start();
	void receive(ByteView packet);
	void encryptionChanged(uint8_t status);
	
private:
	void pair();
	void send(uint8_t command, ByteView payload);
	
public:
	bool encrypted() const { return _state == State::Encrypted; }
	bool bonded() const    { return _bond.has_value(); }
	
private:
	VirtualPeer& _peer;
	
	std::optional<Key128> _bond;
	State _state = State::Idle;
	
	std::optional<SmpKeyPair> _keyPair;
	std::array<uint8_t, 64> _publicKey {}, _remotePublicKey {};
	std::array<uint8_t, 32> _secret {};
	
	std::array<uint8_t, 3> _remoteIo {};
	Key128 _nonce {}, _confirm {}, _remoteCheck {}, _ltk {};
};

} // namespace m5emulator::bluetooth
