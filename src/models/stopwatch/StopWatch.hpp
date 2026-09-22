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

#include "models/Device.hpp"
#include "StopWatchFlash.hpp"
#include "stopwatch_shared.h"

namespace m5emulator::stopwatch {

void resetShared(void* memory);
void exchangeShared(void* memory, DeviceState& state);

inline constexpr std::array<std::string_view, 2> StopWatchButtons { "A", "B" };

inline constexpr std::array<std::string_view, 4> QemuArguments { "-m", "8M", "-global", "driver=ssi_psram,property=is_octal,value=true" };

inline constexpr Device StopWatch {
	.id = "stopwatch",
	.name = "M5STACK STOPWATCH",
	.label = "STOPWATCH",
	.qemuMachine = "m5stopwatch",
	.screen = { M5StopWatchWidth, M5StopWatchHeight, true },
	.buttons = StopWatchButtons,
	.powerButton = M5StopWatchButtonPower,
	.homeButtons = M5StopWatchButtonA | M5StopWatchButtonB,
	.touch = true,
	.motion = true,
	.powerLed = true,
	.vibration = true,
	.flash = { FlashSize, 0, PartitionOffset, ApplicationOffset, ApplicationSize, PartitionTable, "esp32s3/bootloader.bin", 9 },
	.qemuArguments = QemuArguments,
	.audioDevice = "es8311",
	.sharedEnvironment = "M5STOPWATCH_SHARED",
	.sharedSize = sizeof(M5StopWatchShared),
	.resetShared = resetShared,
	.exchangeShared = exchangeShared
};

} // namespace m5emulator::stopwatch
