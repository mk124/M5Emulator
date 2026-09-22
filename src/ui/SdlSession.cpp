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

#include "SdlSession.hpp"

#include <stdexcept>
#include <string>

#include <SDL.h>

namespace m5emulator {

SdlSession::SdlSession(bool headless)
{
	SDL_SetMainReady();
	if (SDL_Init(headless ? SDL_INIT_TIMER : SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
	{
		throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
	}
}

SdlSession::~SdlSession() { SDL_Quit(); }

} // namespace m5emulator
