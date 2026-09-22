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

#include <stdint.h>

enum
{
	M5StopWatchMagic = 0x4D355357,
	M5StopWatchVersion = 3,
	
	M5StopWatchWidth = 466,
	M5StopWatchHeight = 466
};

enum
{
	M5StopWatchButtonA = 1 << 0,
	M5StopWatchButtonB = 1 << 1,
	M5StopWatchButtonPower = 1 << 2
};

/* Native host layout. Both processes must flock(LOCK_EX) the shared file
 * during access, releasing it before rendering or doing other work.
 * The frontend initializes the header and owns buttons, touch, motion and the brightness simulation setting.
 * QEMU owns feedback, counters and native-endian RGB565 pixels.
 * updates is a change version (possibly a pixel-write count), not a frame
 * count. Publish it with the corresponding pixels while holding the lock. */
typedef struct M5StopWatchShared
{
	uint32_t magic, version, width, height;
	
	uint32_t buttons; /* M5StopWatchButton* bitmask. */
	int32_t touchX, touchY;
	uint32_t touchDown;
	float accelerationG[3], angularVelocityDps[3]; /* BMI270 sensor axes X/Y/Z. */
	
	uint16_t vibration, powerLed; /* Motor drive 0..65535; LED 0/1. */
	
	uint32_t completedWindows; /* Completed visible GRAM window writes. */
	uint64_t updates, guestTimeNs;
	uint64_t activeRefreshes; /* TE intervals containing completed window writes. */
	uint16_t pixels[M5StopWatchWidth * M5StopWatchHeight];
	
	uint32_t simulateBrightness; /* Host presentation option, 0/1. */
} M5StopWatchShared;
