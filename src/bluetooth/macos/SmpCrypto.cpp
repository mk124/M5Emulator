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

#include "../SmpCrypto.hpp"

#include <cstdlib>

#include <CommonCrypto/CommonCryptor.h>

namespace m5emulator::bluetooth {

Key128 aesEncryptBlock(const Key128& key, const Key128& block)
{
	Key128 result;
	size_t written = 0;
	const CCCryptorStatus status = CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key.data(), key.size(), nullptr, block.data(), block.size(), result.data(), result.size(), &written);
	require(status == kCCSuccess && written == result.size(), "AES encryption failed");
	return result;
}

void randomBytes(std::span<uint8_t> bytes)
{
	arc4random_buf(bytes.data(), bytes.size());
}

} // namespace m5emulator::bluetooth
