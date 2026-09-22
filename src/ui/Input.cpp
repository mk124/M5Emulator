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

#include "Input.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>

#include <SDL_keycode.h>
#include <SDL_mouse.h>
#include <SDL_rect.h>

namespace m5emulator {

static SDL_Point screenPixel(int x, int y, const SDL_Rect& rect, const Screen& screen)
{
	if (rect.w <= 0 || rect.h <= 0) return { -1, -1 };
	const auto coordinate = [] (int64_t position, int extent, int pixels) {
		if (position < 0) return -1;
		if (position >= extent) return pixels;
		return static_cast<int>(position * pixels / extent);
	};
	return { coordinate(int64_t(x) - rect.x, rect.w, screen.width), coordinate(int64_t(y) - rect.y, rect.h, screen.height) };
}

using Clock = std::chrono::steady_clock;

// Short taps must survive a host frame and the firmware's button debounce.
static constexpr auto MinimumPress = std::chrono::milliseconds(50);
static constexpr auto HomePress = std::chrono::seconds(1);

void Input::reset()
{
	SDL_CaptureMouse(SDL_FALSE);
	*this = {};
}

uint32_t Input::buttons() const
{
	const auto now = Clock::now();
	uint32_t pressed = _keyButtons | _mouseButtons;
	for (std::size_t i = 0; i < _buttonPressUntil.size(); ++i)
	{
		if (now < _buttonPressUntil[i]) pressed |= 1u << i;
	}
	if (now < _homeUntil) pressed |= _homeButtons;
	return pressed;
}

uint32_t Input::touchDown() const { return _touchDown || Clock::now() < _touchUntil; }

void Input::handle(const SDL_Event& event, const Device& device, const SDL_Rect& screenRect, std::span<const SDL_Rect> buttonRects)
{
	if (device.motion) handleMotion(event);
	if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
	{
		const SDL_Keycode key = event.key.keysym.sym;
		if (event.type == SDL_KEYDOWN && key == SDLK_HOME && !event.key.repeat)
		{
			_homeUntil = Clock::now() + HomePress;
			_homeButtons = device.homeButtons;
		}
		
		uint32_t bit = 0;
		for (std::size_t i = 0; i < device.buttons.size(); ++i)
		{
			if (key == SDLK_1 + static_cast<int>(i) || key == SDLK_KP_1 + static_cast<int>(i)) bit = 1u << i;
		}
		if (key == SDLK_BACKQUOTE) bit = device.powerButton;
		
		if (event.type == SDL_KEYDOWN)
		{
			_keyButtons |= bit;
			if (bit && !event.key.repeat) _buttonPressUntil[std::countr_zero(bit)] = Clock::now() + MinimumPress;
		}
		else _keyButtons &= ~bit;
	}
	else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT)
	{
		SDL_CaptureMouse(SDL_TRUE);
		
		const SDL_Point point { event.button.x, event.button.y };
		_mouseButtons = 0;
		for (std::size_t i = 0; i < buttonRects.size(); ++i)
		{
			if (SDL_PointInRect(&point, &buttonRects[i]))
			{
				_mouseButtons |= 1u << i;
				_buttonPressUntil[i] = Clock::now() + MinimumPress;
			}
		}
		
		const auto [touchX, touchY] = screenPixel(point.x, point.y, screenRect, device.screen);
		_dragging = device.touch && device.screen.contains(touchX, touchY);
		_touchDown = _dragging;
		if (_dragging)
		{
			_touchX = touchX;
			_touchY = touchY;
			_touchUntil = Clock::now() + MinimumPress;
		}
	}
	else if (event.type == SDL_MOUSEMOTION && _dragging)
	{
		const auto [touchX, touchY] = screenPixel(event.motion.x, event.motion.y, screenRect, device.screen);
		_touchDown = device.touch && device.screen.contains(touchX, touchY);
		if (!_touchDown) _touchUntil = {};
		
		_touchX = std::clamp(touchX, 0, device.screen.width - 1);
		_touchY = std::clamp(touchY, 0, device.screen.height - 1);
	}
	else if ((event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) ||
	         (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST))
	{
		SDL_CaptureMouse(SDL_FALSE);
		_mouseButtons = 0;
		_touchDown = 0;
		_dragging = false;
		
		if (event.type == SDL_WINDOWEVENT)
		{
			_keyButtons = 0;
			_buttonPressUntil.fill({});
			_homeUntil = {};
			_touchUntil = {};
		}
	}
}

} // namespace m5emulator
