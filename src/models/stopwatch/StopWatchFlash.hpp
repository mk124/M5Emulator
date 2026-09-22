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
#include <string_view>

namespace m5emulator::stopwatch {

inline constexpr std::size_t FlashSize = 16 * 1024 * 1024;
inline constexpr std::size_t PartitionOffset = 0x8000;
inline constexpr std::size_t ApplicationOffset = 0x20000, ApplicationSize = 0x4F0000;

inline constexpr auto PartitionTable = [] {
	std::array<uint8_t, 8 * 32> bytes {};
	std::size_t entry = 0;
	const auto partition = [&] (std::string_view name, uint8_t type, uint8_t subtype, uint32_t offset, uint32_t size) {
		bytes[entry] = 0xAA;
		bytes[entry + 1] = 0x50;
		bytes[entry + 2] = type;
		bytes[entry + 3] = subtype;
		for (std::size_t i = 0; i < 4; ++i)
		{
			bytes[entry + 4 + i] = static_cast<uint8_t>(offset >> (i * 8));
			bytes[entry + 8 + i] = static_cast<uint8_t>(size >> (i * 8));
		}
		for (std::size_t i = 0; i < name.size(); ++i) bytes[entry + 12 + i] = name[i];
		entry += 32;
	};
	
	partition("nvs", 0x01, 0x02, 0x9000, 0x4000);
	partition("otadata", 0x01, 0x00, 0xD000, 0x2000);
	partition("phy_init", 0x01, 0x01, 0xF000, 0x1000);
	partition("ota_0", 0x00, 0x10, ApplicationOffset, ApplicationSize);
	partition("ota_1", 0x00, 0x11, ApplicationOffset + ApplicationSize, ApplicationSize);
	partition("storage", 0x01, 0x81, 0xA00000, 0x400000);
	partition("coredump", 0x01, 0x03, 0xE00000, 0x10000);
	
	// MD5 of the seven entries above; update it when changing this fixed layout.
	constexpr std::array<uint8_t, 16> Md5 { 0xDF, 0x99, 0x5F, 0x29, 0xB4, 0xC9, 0x12, 0xED, 0xD8, 0x50, 0x30, 0x44, 0xE0, 0xC7, 0x12, 0xAE };
	bytes[entry] = bytes[entry + 1] = 0xEB;
	std::fill_n(bytes.begin() + entry + 2, 14, 0xFF);
	std::copy(Md5.begin(), Md5.end(), bytes.begin() + entry + 16);
	return bytes;
}();

} // namespace m5emulator::stopwatch
