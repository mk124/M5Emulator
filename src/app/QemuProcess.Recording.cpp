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

#include "QemuProcess.hpp"

#include <cerrno>
#include <cstddef>
#include <stdexcept>
#include <system_error>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "VideoRecorder.hpp"
#include "recording_audio.h"

namespace m5emulator {

static constexpr uint64_t RecordingTimeoutNs = 2000000000;
static constexpr int RecordingPollIntervalMs = 20;

void QemuProcess::recordAudio(bool enabled, VideoRecorder& recorder)
{
	if (_recordingSocket[0] < 0) throw std::runtime_error("No QEMU audio connection");
	drainRecordedAudio(recorder);
	const uint8_t command = enabled;
	if (send(_recordingSocket[0], &command, sizeof(command), 0) != sizeof(command)) throw std::system_error(errno, std::generic_category(), "request audio recording");
	if (!enabled) return; // Stop acknowledgement is polled by the UI; never block it while saving.
	const uint64_t deadline = m5RecordingTimeNs() + RecordingTimeoutNs;
	while (m5RecordingTimeNs() < deadline)
	{
		pollfd descriptor { _recordingSocket[0], POLLIN, 0 };
		if (poll(&descriptor, 1, RecordingPollIntervalMs) < 0 && errno != EINTR) throw std::system_error(errno, std::generic_category(), "wait for audio recording");
		const uint32_t status = drainRecordedAudio(recorder);
		if (status == (enabled ? M5RecordingStarted : M5RecordingStopped)) return;
		if (status == M5RecordingFailed) throw std::runtime_error("QEMU could not capture speaker audio");
	}
	throw std::runtime_error("QEMU did not acknowledge audio recording; rebuild the bundled QEMU");
}

uint32_t QemuProcess::drainRecordedAudio(VideoRecorder& recorder)
{
	if (_recordingSocket[0] < 0) return 0;
	uint32_t status = 0;
	M5RecordingAudio packet;
	for (;;)
	{
		const ssize_t bytes = recv(_recordingSocket[0], &packet, sizeof(packet), 0);
		if (bytes < 0)
		{
			if (errno == EINTR) continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) return status;
			throw std::system_error(errno, std::generic_category(), "read recording audio");
		}
		if (bytes == 0) return status;
		if (bytes < static_cast<ssize_t>(offsetof(M5RecordingAudio, samples)) || packet.frames > M5RecordingAudioFrames ||
		    bytes != static_cast<ssize_t>(offsetof(M5RecordingAudio, samples) + packet.frames * sizeof(int16_t)))
		{
			throw std::runtime_error("Invalid QEMU recording audio packet");
		}
		if (packet.kind == M5RecordingPcm) recorder.appendAudio(packet);
		else status = packet.kind;
	}
}

} // namespace m5emulator
