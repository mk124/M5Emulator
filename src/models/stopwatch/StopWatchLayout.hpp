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

#include <SDL_rect.h>

#include "models/Device.hpp"

namespace m5emulator {

struct StopWatchLayout
{
	static constexpr int ScreenMargin = 24;
	
	const Device& device = findDevice("stopwatch");
	int width = 0, height = 0;
	SDL_Rect screenRect {}, selectorRect {};
	std::array<SDL_Rect, 2> buttonRects {};
	SDL_Point ledLabel {}, ledCenter {}, vibrationLabel {};
	SDL_Rect vibrationRect {};
	int statisticsTop = 0, helpTop = 0;
	SDL_Point toolbar {};
	int toolbarWidth = 0;
	
	StopWatchLayout();
};

} // namespace m5emulator
