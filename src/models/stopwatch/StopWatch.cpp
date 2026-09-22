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

#include "StopWatch.hpp"

#include <algorithm>
#include <iterator>
#include <limits>

#include "StopWatchFlash.hpp"
#include "stopwatch_shared.h"

namespace m5emulator {

namespace stopwatch {

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(offsetof(M5StopWatchShared, accelerationG) == 32);
static_assert(offsetof(M5StopWatchShared, vibration) == 56);
static_assert(offsetof(M5StopWatchShared, updates) == 64);
static_assert(offsetof(M5StopWatchShared, pixels) == 88);
static_assert(offsetof(M5StopWatchShared, simulateBrightness) == 434400);
static_assert(sizeof(M5StopWatchShared) == 434408);

void resetShared(void* memory)
{
	auto& shared = *static_cast<M5StopWatchShared*>(memory);
	shared = {};
	shared.magic = M5StopWatchMagic;
	shared.version = M5StopWatchVersion;
	shared.width = M5StopWatchWidth;
	shared.height = M5StopWatchHeight;
	shared.touchX = shared.touchY = -1;
	shared.accelerationG[2] = 1;
}

void exchangeShared(void* memory, DeviceState& state)
{
	auto& shared = *static_cast<M5StopWatchShared*>(memory);
	if (shared.magic != M5StopWatchMagic || shared.version != M5StopWatchVersion || shared.width != M5StopWatchWidth || shared.height != M5StopWatchHeight)
	{
		throw std::runtime_error("QEMU shared state has an incompatible StopWatch header");
	}
	shared.simulateBrightness = state.simulateBrightness;
	shared.buttons = state.input.buttons;
	shared.touchX = state.input.touchX;
	shared.touchY = state.input.touchY;
	shared.touchDown = state.input.touchDown;
	std::ranges::copy(state.input.accelerationG, shared.accelerationG);
	std::ranges::copy(state.input.angularVelocityDps, shared.angularVelocityDps);
	state.feedback = { shared.buttons, shared.vibration, shared.powerLed, shared.guestTimeNs, shared.activeRefreshes };
	state.completedWindows = shared.completedWindows;
	if (state.updates != shared.updates)
	{
		state.pixels.assign(std::begin(shared.pixels), std::end(shared.pixels));
		state.updates = shared.updates;
	}
}

} // namespace stopwatch

} // namespace m5emulator
