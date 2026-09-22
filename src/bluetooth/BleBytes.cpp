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

#include "BleBytes.hpp"

namespace m5emulator::bluetooth {

std::string uuidString(ByteView littleEndian)
{
	require(littleEndian.size() == 2 || littleEndian.size() == 16, "invalid GATT UUID length");
	
	static constexpr char HexDigits[] = "0123456789ABCDEF";
	std::string result;
	for (size_t i = 0; i < littleEndian.size(); ++i)
	{
		if (littleEndian.size() == 16 && (i == 4 || i == 6 || i == 8 || i == 10)) result += '-';
		const uint8_t byte = littleEndian[littleEndian.size() - i - 1];
		result += HexDigits[byte >> 4];
		result += HexDigits[byte & 15];
	}
	return result;
}

} // namespace m5emulator::bluetooth
