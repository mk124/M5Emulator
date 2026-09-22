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

#include <filesystem>
#include <string>

#include "models/FlashLayout.hpp"

namespace m5emulator {

void validateFirmware(const std::filesystem::path& path, const FlashLayout& layout);
void validateFlash(const std::filesystem::path& path, const FlashLayout& layout);
void writeFlash(const std::filesystem::path& source, const std::filesystem::path& destination, const FlashLayout& layout);
std::string flashDigest(const std::filesystem::path& path, const FlashLayout& layout);

} // namespace m5emulator
