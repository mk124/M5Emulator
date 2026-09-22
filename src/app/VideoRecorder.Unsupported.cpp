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

#include "VideoRecorder.hpp"
#include "VideoRecorderState.hpp"

#include <stdexcept>

namespace m5emulator {

VideoRecorder::VideoRecorder() = default;
VideoRecorder::~VideoRecorder() = default;

void VideoRecorder::start(const std::filesystem::path&, bool, const Screen&)
{
	throw std::runtime_error("MP4 recording requires macOS");
}

void VideoRecorder::appendAudio(const M5RecordingAudio&) {}
void VideoRecorder::appendFrame(std::span<const uint16_t>) {}
void VideoRecorder::finish(uint64_t) {}
bool VideoRecorder::pollFinished() { return false; }

} // namespace m5emulator
