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

#include "Test.hpp"

#include <cmath>
#include <exception>
#include <iostream>

#include <SDL.h>

#include "ui/Input.hpp"
#include "models/stopwatch/StopWatchLayout.hpp"

namespace m5emulator {

static const StopWatchLayout DefaultLayout;

static void dispatch(Input& input, SDL_Event& event, const StopWatchLayout& layout = DefaultLayout)
{
	CHECK(SDL_PushEvent(&event) == 1);
	while (SDL_PollEvent(&event)) input.handle(event, layout.device, layout.screenRect, layout.buttonRects);
}

static void key(Input& input, SDL_Keycode code, bool pressed, bool repeat = false)
{
	SDL_Event event {};
	event.type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
	event.key.keysym.sym = code;
	event.key.repeat = repeat;
	dispatch(input, event);
}

static bool near(float value, float expected) { return std::abs(value - expected) < 0.0001f; }

static void tiltProducesGravityAndAngularVelocity()
{
	Input input;
	InputState sample {};
	key(input, SDLK_RIGHT, true);
	sample = input.sample(1);
	CHECK(near(sample.accelerationG[0], 0));
	CHECK(near(sample.accelerationG[1], 1));
	CHECK(near(sample.accelerationG[2], 0));
	CHECK(near(sample.angularVelocityDps[0], 90));
	CHECK(near(sample.angularVelocityDps[1], 0));
	CHECK(near(sample.angularVelocityDps[2], 0));
	
	key(input, SDLK_RIGHT, false);
	sample = input.sample(1);
	CHECK(near(sample.accelerationG[1], 1));
	CHECK(near(sample.accelerationG[2], 0));
	CHECK(near(sample.angularVelocityDps[0], 0));
	
	key(input, SDLK_0, true);
	key(input, SDLK_UP, true);
	sample = input.sample(1);
	CHECK(near(sample.accelerationG[0], -1));
	CHECK(near(sample.accelerationG[1], 0));
	CHECK(near(sample.accelerationG[2], 0));
	CHECK(near(sample.angularVelocityDps[1], 90));
	
	key(input, SDLK_0, true);
	key(input, SDLK_e, true);
	sample = input.sample(1);
	CHECK(near(sample.accelerationG[0], 0));
	CHECK(near(sample.accelerationG[1], 0));
	CHECK(near(sample.accelerationG[2], 1));
	CHECK(near(sample.angularVelocityDps[2], 90));
	std::cout << "PASS tilt, retained gravity and yaw\n";
}

static void opposingKeysAndFocusLossStopMotion()
{
	Input input;
	InputState sample {};
	key(input, SDLK_RIGHT, true);
	key(input, SDLK_LEFT, true);
	key(input, SDLK_q, true);
	key(input, SDLK_e, true);
	sample = input.sample(1);
	CHECK(near(sample.accelerationG[2], 1));
	for (float speed : sample.angularVelocityDps) CHECK(near(speed, 0));
	
	key(input, SDLK_LEFT, false);
	key(input, SDLK_q, false);
	key(input, SDLK_SPACE, true);
	key(input, SDLK_1, true);
	key(input, SDLK_HOME, true);
	SDL_Event event {};
	event.type = SDL_WINDOWEVENT;
	event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
	dispatch(input, event);
	sample = input.sample(1.0f / 24);
	CHECK(near(sample.accelerationG[0], 0));
	CHECK(near(sample.accelerationG[1], 0));
	CHECK(near(sample.accelerationG[2], 1));
	for (float speed : sample.angularVelocityDps) CHECK(near(speed, 0));
	CHECK(sample.buttons == 0);
	std::cout << "PASS opposite keys and focus loss\n";
}

static void shakingAndResetDoNotCorruptRestingPose()
{
	Input input;
	InputState sample {};
	key(input, SDLK_SPACE, true);
	bool positive = false, negative = false;
	for (int frame = 0; frame < 60; ++frame)
	{
		sample = input.sample(1.0f / 60);
		positive |= sample.accelerationG[0] > 0.0001f;
		negative |= sample.accelerationG[0] < -0.0001f;
		CHECK(near(sample.accelerationG[1], 0));
		CHECK(near(sample.accelerationG[2], 1));
		for (float speed : sample.angularVelocityDps) CHECK(near(speed, 0));
	}
	CHECK(positive && negative);
	key(input, SDLK_SPACE, false);
	sample = input.sample(0);
	CHECK(near(sample.accelerationG[0], 0));
	CHECK(near(sample.accelerationG[1], 0));
	CHECK(near(sample.accelerationG[2], 1));
	
	key(input, SDLK_RIGHT, true);
	key(input, SDLK_SPACE, true);
	sample = input.sample(0.5f);
	key(input, SDLK_0, true);
	key(input, SDLK_RIGHT, true, true);
	key(input, SDLK_SPACE, true, true);
	for (int frame = 0; frame < 60; ++frame)
	{
		sample = input.sample(1.0f / 60);
		CHECK(near(sample.accelerationG[0], 0));
		CHECK(near(sample.accelerationG[1], 0));
		CHECK(near(sample.accelerationG[2], 1));
		for (float speed : sample.angularVelocityDps) CHECK(near(speed, 0));
	}
	
	key(input, SDLK_RIGHT, false);
	key(input, SDLK_RIGHT, true);
	sample = input.sample(0.5f);
	CHECK(near(sample.accelerationG[1], 0.70710678f));
	CHECK(near(sample.accelerationG[2], 0.70710678f));
	CHECK(near(sample.angularVelocityDps[0], 90));
	std::cout << "PASS shake, reset and key repeat\n";
}

static void touchFollowsTheStopWatchScreen()
{
	Input input;
	SDL_Event event {};
	event.type = SDL_MOUSEBUTTONDOWN;
	event.button.button = SDL_BUTTON_LEFT;
	event.button.x = DefaultLayout.screenRect.x;
	event.button.y = DefaultLayout.screenRect.y;
	dispatch(input, event);
	CHECK(!input.sample(0).touchDown);
	
	event.button.x = DefaultLayout.screenRect.x + 233;
	event.button.y = DefaultLayout.screenRect.y + 233;
	dispatch(input, event);
	InputState state = input.sample(0);
	CHECK(state.touchDown);
	CHECK(state.touchX == 233 && state.touchY == 233);
	
	event.type = SDL_MOUSEMOTION;
	event.motion.x = DefaultLayout.screenRect.x + 465;
	event.motion.y = DefaultLayout.screenRect.y + 233;
	dispatch(input, event);
	state = input.sample(0);
	CHECK(state.touchDown);
	CHECK(state.touchX == 465 && state.touchY == 233);
	
	event.motion.x = DefaultLayout.screenRect.x + 466;
	dispatch(input, event);
	state = input.sample(0);
	CHECK(!state.touchDown);
	CHECK(state.touchX == 465);
	std::cout << "PASS StopWatch touch shape and coordinates\n";
}

static void scaledScreenMapsTouchesToNativePixels()
{
	Input input;
	const SDL_Rect screen { 31, 47, 932, 466 };
	SDL_Event event {};
	event.type = SDL_MOUSEBUTTONDOWN;
	event.button.button = SDL_BUTTON_LEFT;
	event.button.x = 497;
	event.button.y = 280;
	input.handle(event, DefaultLayout.device, screen, {});
	auto state = input.sample(0);
	CHECK(state.touchDown);
	CHECK(state.touchX == 233 && state.touchY == 233);
	
	event.type = SDL_MOUSEMOTION;
	event.motion.x = 962;
	event.motion.y = 280;
	input.handle(event, DefaultLayout.device, screen, {});
	state = input.sample(0);
	CHECK(state.touchDown && state.touchX == 465);
	
	event.motion.x = 30;
	input.handle(event, DefaultLayout.device, screen, {});
	CHECK(!input.sample(0).touchDown);
	
	input.reset();
	event.type = SDL_MOUSEBUTTONDOWN;
	event.button.button = SDL_BUTTON_LEFT;
	event.button.x = 147;
	event.button.y = 163;
	input.handle(event, DefaultLayout.device, { 31, 47, 233, 233 }, {});
	state = input.sample(0);
	CHECK(state.touchDown && state.touchX == 232 && state.touchY == 232);
	std::cout << "PASS scaled touch coordinates and outside edge\n";
}

static void stopWatchButtonsMatchTheirVisiblePositions()
{
	Input input;
	key(input, SDLK_1, true);
	CHECK(input.buttons() == 1);
	
	input = {};
	SDL_Event event {};
	event.type = SDL_MOUSEBUTTONDOWN;
	event.button.button = SDL_BUTTON_LEFT;
	const auto& button = DefaultLayout.buttonRects[1];
	event.button.x = button.x + button.w / 2;
	event.button.y = button.y + button.h / 2;
	dispatch(input, event);
	CHECK(input.buttons() == 2);
	
	input = {};
	key(input, SDLK_BACKQUOTE, true);
	CHECK(input.buttons() == 4);
	input = {};
	key(input, SDLK_3, true);
	CHECK(input.buttons() == 0);
	std::cout << "PASS StopWatch button locations and key mapping\n";
}

} // namespace m5emulator

int main()
{
	try
	{
		CHECK(SDL_Init(SDL_INIT_EVENTS) == 0);
		m5emulator::tiltProducesGravityAndAngularVelocity();
		m5emulator::opposingKeysAndFocusLossStopMotion();
		m5emulator::shakingAndResetDoNotCorruptRestingPose();
		m5emulator::touchFollowsTheStopWatchScreen();
		m5emulator::scaledScreenMapsTouchesToNativePixels();
		m5emulator::stopWatchButtonsMatchTheirVisiblePositions();
		SDL_Quit();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		SDL_Quit();
		return 1;
	}
}
