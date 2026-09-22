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

#include "SmpSession.hpp"

#include <iostream>
#include <stdexcept>

#include "SmpCrypto.hpp"
#include "VirtualPeer.hpp"

namespace m5emulator::bluetooth {

static constexpr std::array<uint8_t, 3> LocalIo { 3, 0, 9 };

SmpSession::SmpSession(VirtualPeer& peer) : _peer(peer) {}

void SmpSession::reset()
{
	_state = State::Idle;
	_keyPair.reset();
	_secret = {};
	_nonce = _confirm = _remoteCheck = _ltk = {};
}

void SmpSession::start()
{
	if (_state != State::Idle) return;
	
	if (_bond)
	{
		_state = State::Restoring;
		_peer.requestEncryption(*_bond);
	}
	else pair();
}

void SmpSession::receive(ByteView packet)
{
	require(!packet.empty(), "empty SMP packet");
	const ByteView payload = packet.subspan(1);
	if (packet[0] == 0x0B)
	{
		require(payload.size() == 1 && !(payload[0] & 0x04), "SMP peer requires unsupported MITM authentication");
		start();
		return;
	}
	if (packet[0] == 5) throw std::runtime_error("original NimBLE rejected SMP pairing");
	
	switch (_state)
	{
		case State::Response: {
			require(packet[0] == 2 && payload.size() == 6, "unexpected SMP pairing response");
			require(payload[0] <= 4 && payload[1] == 0 && (payload[2] & 0x0F) == 9 && payload[3] == 16 && payload[4] == 0 && payload[5] == 0, "SMP peer does not support the selected SC Just Works policy");
			_remoteIo = fixedBytes<3>(payload.first(3));
			_state = State::PublicKey;
			send(0x0C, _publicKey);
			break;
		}
		case State::PublicKey: {
			require(packet[0] == 0x0C && payload.size() == 64, "unexpected SMP public key");
			_remotePublicKey = fixedBytes<64>(payload);
			require(_remotePublicKey != _publicKey, "SMP reflected public key");
			_secret = _keyPair->sharedSecret(payload);
			_state = State::Confirm;
			break;
		}
		case State::Confirm: {
			require(packet[0] == 3 && payload.size() == 16, "unexpected SMP confirmation");
			_confirm = fixedBytes<16>(payload);
			_state = State::Random;
			send(4, _nonce);
			break;
		}
		case State::Random: {
			require(packet[0] == 4 && payload.size() == 16, "unexpected SMP nonce");
			const Key128 remoteNonce = fixedBytes<16>(payload);
			const Key128 expected = smpConfirm(ByteView(_remotePublicKey).first(32), ByteView(_publicKey).first(32), remoteNonce);
			require(equalKey(_confirm, expected), "SMP confirmation mismatch");
			
			const auto [macKey, ltk] = smpKeys(_secret, _nonce, remoteNonce, _peer.localAddress(), _peer.remoteAddress());
			_ltk = ltk;
			_remoteCheck = smpCheck(macKey, remoteNonce, _nonce, _remoteIo, _peer.remoteAddress(), _peer.localAddress());
			_state = State::Check;
			send(0x0D, smpCheck(macKey, _nonce, remoteNonce, LocalIo, _peer.localAddress(), _peer.remoteAddress()));
			break;
		}
		case State::Check: {
			require(packet[0] == 0x0D && payload.size() == 16, "unexpected SMP DHKey check");
			require(equalKey(_remoteCheck, fixedBytes<16>(payload)), "SMP DHKey check mismatch");
			_state = State::Encrypting;
			_peer.requestEncryption(_ltk);
			break;
		}
		default: throw std::runtime_error("unexpected SMP packet for current security state");
	}
}

void SmpSession::encryptionChanged(uint8_t status)
{
	require(_state == State::Encrypting || _state == State::Restoring, "unsolicited virtual encryption result");
	if (status == 6 && _state == State::Restoring)
	{
		// A fresh NVS image no longer has our bond. Authentication failures never fall back.
		_bond.reset();
		pair();
		return;
	}
	require(status == 0, "original NimBLE key did not match the virtual peer");
	
	const bool restored = _state == State::Restoring;
	if (!restored) _bond = _ltk;
	_state = State::Encrypted;
	_keyPair.reset();
	_secret = {};
	_nonce = _confirm = _remoteCheck = _ltk = {};
	
	std::cout << "BLE bridge: " << (restored ? "bond restored" : "SC pairing complete; original NimBLE LTK matched") << '\n'
	          << std::flush;
	_peer.securityReady();
}

void SmpSession::pair()
{
	_keyPair.emplace();
	_publicKey = _keyPair->publicKey();
	randomBytes(_nonce);
	_state = State::Response;
	
	// SC Just Works, 128-bit key, bonding, no optional identity/signing distribution.
	send(1, std::array<uint8_t, 6> { 3, 0, 9, 16, 0, 0 });
}

void SmpSession::send(uint8_t command, ByteView payload)
{
	Bytes packet { command };
	append(packet, payload);
	_peer.sendL2cap(6, packet);
}

} // namespace m5emulator::bluetooth
