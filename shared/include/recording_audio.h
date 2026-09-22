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
#include <time.h>

/* Local host IPC; both processes use the same native ABI and monotonic clock. */
enum
{
	M5RecordingAudioRate = 48000,
	M5RecordingAudioFrames = 1024,
	
	M5RecordingPcm = 1,
	M5RecordingStarted,
	M5RecordingStopped,
	M5RecordingFailed
};

typedef struct M5RecordingAudio
{
	uint64_t timeNs;
	uint32_t frames, kind;
	
	int16_t samples[M5RecordingAudioFrames];
} M5RecordingAudio;

static inline uint64_t m5RecordingTimeNs(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t) now.tv_sec * 1000000000 + now.tv_nsec;
}
