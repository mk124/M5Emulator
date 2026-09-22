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
#include <iostream>
#include <string_view>

#include <unistd.h>

namespace m5emulator {

static constexpr int MaxOutputReadsPerFrame = 16;

static bool isLedStateLine(std::string_view line)
{
	while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
	if (line.starts_with("\x1B["))
	{
		const size_t end = line.find('m');
		if (end != std::string_view::npos) line.remove_prefix(end + 1);
	}
	if (line.ends_with("\x1B[0m")) line.remove_suffix(4);
	if (!line.starts_with("I (")) return false;
	
	static constexpr std::string_view Message = ") M5PM1_LED: LED Enable Level: ";
	const size_t end = line.find(Message, 3);
	if (end == std::string_view::npos) return false;
	const std::string_view timestamp = line.substr(3, end - 3);
	if (timestamp.empty() || timestamp.find_first_not_of("0123456789") != std::string_view::npos) return false;
	const std::string_view level = line.substr(end + Message.size());
	return level == "High" || level == "Low";
}

static void forwardLine(std::string_view line)
{
	// The display already shows LED state. Preserve firmware errors and all other output.
	if (!isLedStateLine(line)) std::cout << line;
}

void QemuProcess::drainOutput() noexcept
{
	char buffer[4096];
	// Bound work per frame; a verbose guest must not monopolize the frontend loop.
	for (int reads = 0; _outputPipe[0] >= 0 && reads < MaxOutputReadsPerFrame; ++reads)
	{
		const ssize_t count = read(_outputPipe[0], buffer, sizeof(buffer));
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) break;
		_outputLine.append(buffer, count);
		
		size_t start = 0, end;
		while ((end = _outputLine.find('\n', start)) != std::string::npos)
		{
			const std::string_view line = std::string_view(_outputLine).substr(start, end + 1 - start);
			if (_outputPassthrough) std::cout << line;
			else forwardLine(line);
			_outputPassthrough = false;
			start = end + 1;
		}
		_outputLine.erase(0, start);
		if (_outputPassthrough || _outputLine.size() >= sizeof(buffer))
		{
			std::cout << _outputLine;
			_outputLine.clear();
			_outputPassthrough = true;
		}
	}
	std::cout.flush();
}

void QemuProcess::closeOutput() noexcept
{
	if (_outputPipe[1] >= 0) close(_outputPipe[1]);
	_outputPipe[1] = -1;
	drainOutput();
	if (_outputPipe[0] >= 0) close(_outputPipe[0]);
	_outputPipe[0] = -1;
	if (_outputPassthrough) std::cout << _outputLine;
	else forwardLine(_outputLine);
	_outputLine.clear();
	_outputPassthrough = false;
	std::cout.flush();
}

} // namespace m5emulator
