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
#include <string>
#include <vector>

#include "GattDescriptor.hpp"

namespace m5emulator::bluetooth {

struct GattCharacteristic
{
	// NimBLE ATT permissions; characteristic properties alone do not describe access security.
	static constexpr uint8_t Read = 0x01, Write = 0x02, ReadEncrypted = 0x04, WriteEncrypted = 0x20;
	static constexpr uint8_t AuthenticatedOrAuthorized = 0xD8;
	
	uint16_t declaration = 0, handle = 0;
	uint8_t properties = 0;
	std::string uuid;
	std::vector<GattDescriptor> descriptors;
	uint8_t permissions = 0, minimumKeySize = 0;
	
	bool operator==(const GattCharacteristic&) const = default;
};

} // namespace m5emulator::bluetooth
