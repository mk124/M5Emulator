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

#include <utility>

#include "BleBytes.hpp"

namespace m5emulator::bluetooth {

// Platform cryptographic primitives; SMP byte order and key derivation stay shared.
Key128 aesEncryptBlock(const Key128& key, const Key128& block);
void randomBytes(std::span<uint8_t> bytes);

Key128 aesCmac(const Key128& key, ByteView message);
Key128 smpConfirm(ByteView firstX, ByteView secondX, const Key128& nonce);
std::pair<Key128, Key128> smpKeys(ByteView sharedSecret, const Key128& firstNonce, const Key128& secondNonce, const Address& firstAddress, const Address& secondAddress);
Key128 smpCheck(const Key128& macKey, const Key128& firstNonce, const Key128& secondNonce, ByteView io, const Address& firstAddress, const Address& secondAddress);

} // namespace m5emulator::bluetooth
