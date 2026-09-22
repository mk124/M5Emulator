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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace m5emulator::bluetooth {

using Bytes = std::vector<uint8_t>;
using ByteView = std::span<const uint8_t>;
using Key128 = std::array<uint8_t, 16>;
using Address = std::array<uint8_t, 7>; // Little-endian address followed by address type.

inline void require(bool valid, const char* message)
{
	if (!valid) throw std::runtime_error(message);
}

inline uint16_t readLe16(ByteView bytes)
{
	require(bytes.size() >= 2, "truncated BLE integer");
	return static_cast<uint16_t>(bytes[0] | static_cast<unsigned>(bytes[1]) << 8);
}

inline void appendLe16(Bytes& bytes, uint16_t value)
{
	bytes.push_back(value);
	bytes.push_back(value >> 8);
}

inline void append(Bytes& bytes, ByteView tail)        { bytes.insert(bytes.end(), tail.begin(), tail.end()); }
inline void appendReverse(Bytes& bytes, ByteView tail) { bytes.insert(bytes.end(), tail.rbegin(), tail.rend()); }

template<size_t Size>
std::array<uint8_t, Size> fixedBytes(ByteView bytes)
{
	require(bytes.size() == Size, "invalid BLE field length");
	std::array<uint8_t, Size> result;
	std::ranges::copy(bytes, result.begin());
	return result;
}

template<size_t Size>
std::array<uint8_t, Size> reversed(const std::array<uint8_t, Size>& bytes)
{
	std::array<uint8_t, Size> result;
	std::ranges::reverse_copy(bytes, result.begin());
	return result;
}

inline bool equalKey(const Key128& left, const Key128& right)
{
	uint8_t difference = 0;
	for (size_t i = 0; i < left.size(); ++i) difference |= left[i] ^ right[i];
	return difference == 0;
}

std::string uuidString(ByteView littleEndian);

} // namespace m5emulator::bluetooth
