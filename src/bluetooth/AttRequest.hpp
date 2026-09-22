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

#include <cstddef>
#include <cstdint>
#include <functional>

#include "BleBytes.hpp"

namespace m5emulator::bluetooth {

inline constexpr uint16_t DefaultAttMtu = 23;
inline constexpr uint16_t MaxAttMtu = 498;
inline constexpr size_t MaxAttValueSize = 512;

struct AttRequest
{
	using Completion = std::function<void(uint8_t, ByteView)>;
	
	Bytes packet;
	Completion completed;
	
	Bytes longValue;
	uint8_t prepareError = 0;
};

} // namespace m5emulator::bluetooth
