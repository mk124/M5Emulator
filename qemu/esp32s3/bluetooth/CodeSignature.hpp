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
#include <span>
#include <string_view>

#include "CodeReference.hpp"

namespace m5emulator::ble {

struct CodeSignature
{
	std::string_view name;
	uint32_t operation;
	bool iram;
	std::string_view code, mask;
	std::span<const uint16_t> calls;
	std::span<const CodeReference> references;
};

} // namespace m5emulator::ble
