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

#ifdef __APPLE__
#import <AVFoundation/AVFoundation.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>

#include "recording_audio.h"
#include "models/Screen.hpp"

namespace m5emulator {

struct VideoRecorderState
{
	static constexpr std::size_t AudioBufferSeconds = 5;
	
	#ifdef __APPLE__
	AVAssetWriter* writer;
	#endif
	Screen screen {};
	uint64_t startNs = 0;
	
	#ifdef __APPLE__
	AVAssetWriterInput* video;
	AVAssetWriterInputPixelBufferAdaptor* pixels;
	#endif
	uint64_t lastVideoNs = 0;
	bool hasVideo = false;
	
	#ifdef __APPLE__
	AVAssetWriterInput* audio;
	#endif
	uint64_t audioFrame = 0;
	// AVAssetWriter may hold audio while it interleaves encoded video.
	std::array<int16_t, M5RecordingAudioRate * AudioBufferSeconds> samples {};
};

} // namespace m5emulator
