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

#include "StopWatchLayout.hpp"

#include "ui/EmulatorControls.hpp"

namespace m5emulator {

static constexpr int GlyphAdvance = Display::GlyphAdvance, GlyphHeight = Display::GlyphHeight;
static constexpr int ButtonWidth = 132, ButtonHeight = 44, ButtonGap = 66;

StopWatchLayout::StopWatchLayout()
{
	width = device.screen.width + ScreenMargin * 2;
	screenRect = { ScreenMargin, ScreenMargin, device.screen.width, device.screen.height };
	
	const int buttonLeft = (width - ButtonWidth * 2 - ButtonGap) / 2;
	const int buttonTop = screenRect.y + screenRect.h + 20;
	buttonRects = { { { buttonLeft, buttonTop, ButtonWidth, ButtonHeight },
	                  { buttonLeft + ButtonWidth + ButtonGap, buttonTop, ButtonWidth, ButtonHeight } } };
	
	const int statusTop = buttonTop + ButtonHeight + 16;
	ledLabel = { ScreenMargin, statusTop };
	ledCenter = { ledLabel.x + GlyphAdvance * 8, ledLabel.y + GlyphHeight };
	vibrationLabel = { width / 2, statusTop };
	const int vibrationX = vibrationLabel.x + GlyphAdvance * 8;
	vibrationRect = { vibrationX, statusTop, width - ScreenMargin - vibrationX, GlyphHeight * 2 };
	
	// The selector fits in the empty corner outside the round screen.
	const SDL_Point selector = EmulatorControls::selectorSize(device);
	constexpr int SelectorMargin = 8;
	selectorRect = { width - SelectorMargin - selector.x, SelectorMargin, selector.x, selector.y };
	statisticsTop = statusTop + 24;
	helpTop = statisticsTop + 32;
	toolbar = { ScreenMargin, helpTop + 56 };
	toolbarWidth = width - ScreenMargin * 2;
	height = toolbar.y + EmulatorControls::Height + ScreenMargin;
}

} // namespace m5emulator
