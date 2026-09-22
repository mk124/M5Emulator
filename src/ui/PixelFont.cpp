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

#include "PixelFont.hpp"

#include <charconv>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace m5emulator {

static constexpr char32_t Replacement = 0xFFFD;
static constexpr std::size_t BmpCodePoints = 0x10000;

static uint32_t parseHex(std::string_view text)
{
	uint32_t value = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
	if (result.ec != std::errc {} || result.ptr != text.data() + text.size()) throw std::runtime_error("Invalid pixel font data");
	return value;
}

static char32_t nextCharacter(std::string_view& text)
{
	const auto first = static_cast<uint8_t>(text.front());
	text.remove_prefix(1);
	if (first < 0x80) return first < 0x20 ? Replacement : first;
	int count = 0;
	if (first >= 0xC2 && first <= 0xDF) count = 2;
	else if (first >= 0xE0 && first <= 0xEF) count = 3;
	else if (first >= 0xF0 && first <= 0xF4) count = 4;
	if (!count || text.size() < static_cast<std::size_t>(count - 1)) return Replacement;
	char32_t code = first & (0x7F >> count);
	for (int i = 0; i < count - 1; ++i)
	{
		const auto byte = static_cast<uint8_t>(text[i]);
		if ((byte & 0xC0) != 0x80) return Replacement;
		code = (code << 6) | (byte & 0x3F);
	}
	text.remove_prefix(count - 1);
	if ((count == 3 && code < 0x800) || code > 0xFFFF || (code >= 0xD800 && code <= 0xDFFF) || count == 4) return Replacement;
	return code;
}

PixelFont::PixelFont(const std::filesystem::path& path) : _glyphs(BmpCodePoints), _widths(BmpCodePoints)
{
	std::ifstream input(path);
	if (!input) throw std::runtime_error("Cannot open the bundled pixel font");
	std::string line;
	while (std::getline(input, line))
	{
		const std::size_t separator = line.find(':');
		if (separator == std::string::npos) throw std::runtime_error("Invalid pixel font record");
		const uint32_t code = parseHex(std::string_view(line).substr(0, separator));
		const std::string_view bitmap = std::string_view(line).substr(separator + 1);
		if (code >= _glyphs.size() || (bitmap.size() != 32 && bitmap.size() != 64)) throw std::runtime_error("Invalid pixel font glyph");
		const std::size_t digits = bitmap.size() / Height;
		_widths[code] = static_cast<uint8_t>(digits * 4);
		for (int row = 0; row < Height; ++row) _glyphs[code][row] = static_cast<uint16_t>(parseHex(bitmap.substr(row * digits, digits)));
	}
	if (input.bad() || !_widths[Replacement] || !_widths['.']) throw std::runtime_error("Incomplete pixel font");
}

PixelFont::Surface PixelFont::render(std::string_view text, int width, int lines) const
{
	if (width < 24 || lines < 1) throw std::invalid_argument("Pixel text needs space for one line and an ellipsis");
	Surface surface(SDL_CreateRGBSurfaceWithFormat(0, width, Height * lines, 32, SDL_PIXELFORMAT_RGBA32), SDL_FreeSurface);
	if (!surface) throw std::runtime_error(SDL_GetError());
	SDL_FillRect(surface.get(), nullptr, 0);
	const uint32_t white = SDL_MapRGBA(surface->format, 255, 255, 255, 255);
	for (int row = 0; row < lines && !text.empty(); ++row)
	{
		std::u32string line;
		int length = 0;
		while (!text.empty())
		{
			std::string_view remaining = text;
			char32_t code = nextCharacter(remaining);
			if (!_widths[code]) code = Replacement;
			if (length + _widths[code] > width) break;
			line.push_back(code);
			length += _widths[code];
			text = remaining;
		}
		
		if (row == lines - 1 && !text.empty())
		{
			while (length + 3 * _widths['.'] > width && !line.empty())
			{
				length -= _widths[line.back()];
				line.pop_back();
			}
			line += U"...";
		}
		
		int x = 0;
		for (char32_t code : line)
		{
			for (int y = 0; y < Height; ++y)
			{
				auto* pixels = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(surface->pixels) + (row * Height + y) * surface->pitch);
				for (int column = 0; column < _widths[code]; ++column)
				{
					if (_glyphs[code][y] & (1u << (_widths[code] - column - 1))) pixels[x + column] = white;
				}
			}
			x += _widths[code];
		}
	}
	return surface;
}

} // namespace m5emulator
