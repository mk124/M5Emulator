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

#include <cmath>
#include <numbers>

#include <SDL_keycode.h>

namespace m5emulator {

static constexpr float TurnRateDps = 90;
static constexpr float ShakeFrequencyHz = 6, ShakeAccelerationG = 2;

InputState Input::sample(float elapsedSeconds)
{
	InputState state;
	state.buttons = buttons();
	state.touchX = _touchX;
	state.touchY = _touchY;
	state.touchDown = touchDown();
	
	std::array<float, 3> angularVelocity {};
	for (unsigned axis = 0; axis < angularVelocity.size(); ++axis)
	{
		const int positive = (_rotationKeys >> (axis * 2)) & 1;
		const int negative = (_rotationKeys >> (axis * 2 + 1)) & 1;
		angularVelocity[axis] = (positive - negative) * TurnRateDps;
	}
	
	const float speed = std::hypot(angularVelocity[0], angularVelocity[1], angularVelocity[2]);
	if (speed > 0)
	{
		std::array<float, 3> axis {};
		for (unsigned i = 0; i < axis.size(); ++i) axis[i] = angularVelocity[i] / speed;
		
		// Rotate gravity into the moving sensor frame, opposite to body rotation.
		const float angle = -speed * elapsedSeconds * std::numbers::pi_v<float> / 180;
		const float cosine = std::cos(angle), sine = std::sin(angle);
		const float projection = axis[0] * _gravity[0] + axis[1] * _gravity[1] + axis[2] * _gravity[2];
		const std::array cross {
			axis[1] * _gravity[2] - axis[2] * _gravity[1],
			axis[2] * _gravity[0] - axis[0] * _gravity[2],
			axis[0] * _gravity[1] - axis[1] * _gravity[0]
		};
		for (unsigned i = 0; i < _gravity.size(); ++i)
		{
			_gravity[i] = _gravity[i] * cosine + cross[i] * sine + axis[i] * projection * (1 - cosine);
		}
		const float magnitude = std::hypot(_gravity[0], _gravity[1], _gravity[2]);
		for (float& value : _gravity) value /= magnitude;
	}
	
	state.accelerationG = _gravity;
	state.angularVelocityDps = angularVelocity;
	if (_shaking)
	{
		_shakePhase = std::fmod(_shakePhase + elapsedSeconds * ShakeFrequencyHz * 2 * std::numbers::pi_v<float>, 2 * std::numbers::pi_v<float>);
		state.accelerationG[0] += ShakeAccelerationG * std::sin(_shakePhase);
	}
	return state;
}

void Input::handleMotion(const SDL_Event& event)
{
	if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
	{
		_rotationKeys = 0;
		_shaking = false;
		_shakePhase = 0;
		return;
	}
	if (event.type != SDL_KEYDOWN && event.type != SDL_KEYUP) return;
	if (event.type == SDL_KEYDOWN && event.key.repeat) return;
	
	const SDL_Keycode key = event.key.keysym.sym;
	const bool pressed = event.type == SDL_KEYDOWN;
	if (pressed && (key == SDLK_0 || key == SDLK_KP_0))
	{
		_gravity = { 0, 0, 1 };
		_rotationKeys = 0;
		_shaking = false;
		_shakePhase = 0;
	}
	if (key == SDLK_SPACE)
	{
		_shaking = pressed;
		if (!pressed) _shakePhase = 0;
	}
	
	// The StopWatch HAL swaps sensor X/Y for its display coordinates.
	uint32_t bit = 0;
	if (key == SDLK_RIGHT) bit = 1 << 0;
	else if (key == SDLK_LEFT) bit = 1 << 1;
	else if (key == SDLK_UP) bit = 1 << 2;
	else if (key == SDLK_DOWN) bit = 1 << 3;
	else if (key == SDLK_e) bit = 1 << 4;
	else if (key == SDLK_q) bit = 1 << 5;
	
	if (pressed) _rotationKeys |= bit;
	else _rotationKeys &= ~bit;
}

} // namespace m5emulator
