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

namespace m5emulator {

struct Screen
{
	int width, height;
	bool round;
	
	constexpr std::size_t pixelCount() const { return static_cast<std::size_t>(width) * height; }
	
	constexpr bool contains(int x, int y) const
	{
		if (x < 0 || y < 0 || x >= width || y >= height) return false;
		if (!round) return true;
		const int64_t dx = 2 * x + 1 - width, dy = 2 * y + 1 - height;
		return dx * dx + dy * dy <= int64_t(width) * width;
	}
};

} // namespace m5emulator
