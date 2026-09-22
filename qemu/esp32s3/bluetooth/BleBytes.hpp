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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

namespace m5emulator::ble {

[[noreturn]] inline void fail(const char* message)
{
	std::fprintf(stderr, "BLE HLE: %s\n", message);
	std::abort();
}

inline uint16_t readLe16(std::span<const uint8_t> bytes)
{
	return static_cast<uint16_t>(bytes[0] | static_cast<unsigned>(bytes[1]) << 8);
}

inline uint32_t readLe32(std::span<const uint8_t> bytes)
{
	return readLe16(bytes) | static_cast<uint32_t>(readLe16(bytes.subspan(2))) << 16;
}

inline void appendLe16(std::vector<uint8_t>& bytes, uint16_t value)
{
	bytes.push_back(value);
	bytes.push_back(value >> 8);
}

inline void tracePacket(const char* direction, std::span<const uint8_t> bytes)
{
	std::fprintf(stderr, "BLE HLE: %s", direction);
	for (const uint8_t byte : bytes) std::fprintf(stderr, " %02X", byte);
	std::fputc('\n', stderr);
}

} // namespace m5emulator::ble
