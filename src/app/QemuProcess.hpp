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

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

#include <sys/types.h>

namespace m5emulator {

struct Options;
class VideoRecorder;

class QemuProcess
{
public:
	QemuProcess() = default;
	~QemuProcess();
	
	QemuProcess(const QemuProcess&) = delete;
	QemuProcess& operator=(const QemuProcess&) = delete;
	
	void start(const Options& options, const std::filesystem::path& flashPath, const std::filesystem::path& sharedPath);
	bool exited(int& status);
	void stop() noexcept;
	
	void recordAudio(bool enabled, VideoRecorder& recorder);
	uint32_t drainRecordedAudio(VideoRecorder& recorder);
	
private:
	void drainOutput() noexcept;
	void closeOutput() noexcept;
	
private:
	pid_t _pid = -1;
	
	std::array<int, 2> _recordingSocket { -1, -1 };
	
	std::array<int, 2> _consolePipe { -1, -1 };
	std::array<int, 2> _outputPipe { -1, -1 };
	std::string _outputLine;
	bool _outputPassthrough = false;
};

} // namespace m5emulator
