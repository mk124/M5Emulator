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

#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <span>

#include "models/Screen.hpp"
#include "recording_audio.h"

namespace m5emulator {

struct VideoRecorderState;

class VideoRecorder
{
public:
	VideoRecorder();
	~VideoRecorder();
	VideoRecorder(const VideoRecorder&) = delete;
	VideoRecorder& operator=(const VideoRecorder&) = delete;
	
	void start(const std::filesystem::path& path, bool audio, const Screen& screen);
	void appendAudio(const M5RecordingAudio& packet);
	void appendFrame(std::span<const uint16_t> pixels);
	void finish(uint64_t endNs = 0);
	bool pollFinished();
	
private:
	void finishWriting(uint64_t elapsed);
	void flushAudio(uint64_t throughNs);
	
public:
	[[nodiscard]] bool finishing() const                    { return _finishing.valid(); }
	[[nodiscard]] bool active() const                       { return static_cast<bool>(_state); }
	[[nodiscard]] const std::filesystem::path& path() const { return _path; }
	
private:
	std::unique_ptr<VideoRecorderState> _state;
	std::filesystem::path _path;
	std::future<void> _finishing;
};

} // namespace m5emulator
