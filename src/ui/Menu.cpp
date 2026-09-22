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

#include "Menu.hpp"

namespace m5emulator {

MenuAction handleMenuInput(const SDL_Event& event, std::optional<std::size_t>& selection, const SDL_Rect& bounds, const SDL_Rect& firstRow, std::size_t count, SDL_Keycode closeKey)
{
	if (!selection) return MenuAction::PassThrough;
	
	if (event.type == SDL_KEYDOWN)
	{
		const SDL_Keycode key = event.key.keysym.sym;
		if (key == SDLK_ESCAPE || (closeKey != SDLK_UNKNOWN && key == closeKey))
		{
			if (!event.key.repeat) selection.reset();
		}
		else if (key == SDLK_UP) selection = (*selection + count - 1) % count;
		else if (key == SDLK_DOWN) selection = (*selection + 1) % count;
		else if (key == SDLK_RETURN || key == SDLK_KP_ENTER)
		{
			if (!event.key.repeat) return MenuAction::Activate;
		}
	}
	else if (event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEBUTTONDOWN)
	{
		const SDL_Point point = event.type == SDL_MOUSEMOTION ? SDL_Point { event.motion.x, event.motion.y } : SDL_Point { event.button.x, event.button.y };
		const bool clicked = event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT;
		if (clicked && !SDL_PointInRect(&point, &bounds)) selection.reset();
		else
		{
			SDL_Rect row = firstRow;
			for (std::size_t i = 0; i < count; ++i, row.y += row.h)
			{
				if (!SDL_PointInRect(&point, &row)) continue;
				selection = i;
				if (clicked) return MenuAction::Activate;
				break;
			}
		}
	}
	else if (event.type == SDL_WINDOWEVENT)
	{
		if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) selection.reset();
		return MenuAction::PassThrough;
	}
	
	return MenuAction::Consumed;
}

} // namespace m5emulator
