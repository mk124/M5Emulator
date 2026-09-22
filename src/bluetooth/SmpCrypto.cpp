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

#include "SmpCrypto.hpp"

#include <algorithm>
#include <array>

namespace m5emulator::bluetooth {

static Key128 doubleBlock(const Key128& block)
{
	Key128 result;
	for (size_t i = 0; i < block.size(); ++i)
	{
		result[i] = static_cast<uint8_t>((block[i] << 1) | (i + 1 < block.size() ? block[i + 1] >> 7 : 0));
	}
	if (block[0] & 0x80) result.back() ^= 0x87;
	return result;
}

Key128 aesCmac(const Key128& key, ByteView message)
{
	// NIST SP 800-38B: CBC-MAC with distinct complete/partial final-block subkeys.
	const Key128 firstSubkey = doubleBlock(aesEncryptBlock(key, {}));
	const Key128 lastSubkey = !message.empty() && message.size() % 16 == 0 ? firstSubkey : doubleBlock(firstSubkey);
	const size_t blocks = std::max<size_t>(1, (message.size() + 15) / 16);
	Key128 state {};
	for (size_t block = 0; block < blocks; ++block)
	{
		const size_t offset = block * 16;
		for (size_t i = 0; i < 16; ++i)
		{
			if (offset + i < message.size()) state[i] ^= message[offset + i];
			else if (offset + i == message.size()) state[i] ^= 0x80;
			if (block + 1 == blocks) state[i] ^= lastSubkey[i];
		}
		state = aesEncryptBlock(key, state);
	}
	return state;
}

Key128 smpConfirm(ByteView firstX, ByteView secondX, const Key128& nonce)
{
	Bytes message;
	appendReverse(message, firstX);
	appendReverse(message, secondX);
	message.push_back(0);
	return reversed(aesCmac(reversed(nonce), message));
}

std::pair<Key128, Key128> smpKeys(ByteView sharedSecret, const Key128& firstNonce, const Key128& secondNonce, const Address& firstAddress, const Address& secondAddress)
{
	// Bluetooth Vol 3, Part H, f5. Public protocol fields use little-endian order.
	static constexpr Key128 Salt { 0x6C, 0x88, 0x83, 0x91, 0xAA, 0xF5, 0xA5, 0x38, 0x60, 0x37, 0x0B, 0xDB, 0x5A, 0x60, 0x83, 0xBE };
	
	Bytes message(sharedSecret.rbegin(), sharedSecret.rend());
	const Key128 temporary = aesCmac(Salt, message);
	
	message = { 0, 0x62, 0x74, 0x6C, 0x65 };
	appendReverse(message, firstNonce);
	appendReverse(message, secondNonce);
	appendReverse(message, firstAddress);
	appendReverse(message, secondAddress);
	append(message, std::array<uint8_t, 2> { 1, 0 });
	
	const Key128 macKey = reversed(aesCmac(temporary, message));
	message[0] = 1;
	return { macKey, reversed(aesCmac(temporary, message)) };
}

Key128 smpCheck(const Key128& macKey, const Key128& firstNonce, const Key128& secondNonce, ByteView io, const Address& firstAddress, const Address& secondAddress)
{
	Bytes message;
	appendReverse(message, firstNonce);
	appendReverse(message, secondNonce);
	append(message, Key128 {}); // Just Works has no passkey or OOB randomizer.
	appendReverse(message, io);
	appendReverse(message, firstAddress);
	appendReverse(message, secondAddress);
	return reversed(aesCmac(reversed(macKey), message));
}

} // namespace m5emulator::bluetooth
