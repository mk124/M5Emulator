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
#include <chrono>
#include <cstdint>
#include <span>

#include <SDL_events.h>
#include <SDL_rect.h>

#include "models/Device.hpp"
#include "models/InputState.hpp"

namespace m5emulator {

class Input
{
public:
	void reset();
	uint32_t buttons() const;
	uint32_t touchDown() const;
	void handle(const SDL_Event& event, const Device& device, const SDL_Rect& screenRect, std::span<const SDL_Rect> buttonRects);
	
	InputState sample(float elapsedSeconds);
	
private:
	void handleMotion(const SDL_Event& event);
	
private:
	uint32_t _keyButtons = 0, _mouseButtons = 0, _homeButtons = 0;
	std::array<std::chrono::steady_clock::time_point, 32> _buttonPressUntil {};
	std::chrono::steady_clock::time_point _homeUntil {};
	
	int32_t _touchX = -1, _touchY = -1;
	uint32_t _touchDown = 0;
	std::chrono::steady_clock::time_point _touchUntil {};
	bool _dragging = false;
	
	std::array<float, 3> _gravity { 0, 0, 1 };
	uint32_t _rotationKeys = 0;
	float _shakePhase = 0;
	bool _shaking = false;
};

} // namespace m5emulator
