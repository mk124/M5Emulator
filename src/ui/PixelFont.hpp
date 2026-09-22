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

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include <SDL.h>

namespace m5emulator {

class PixelFont
{
public:
	using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;
	
	static constexpr int Height = 16;
	
	explicit PixelFont(const std::filesystem::path& path);
	Surface render(std::string_view text, int width, int lines) const;
	
private:
	std::vector<std::array<uint16_t, Height>> _glyphs;
	std::vector<uint8_t> _widths;
};

} // namespace m5emulator
