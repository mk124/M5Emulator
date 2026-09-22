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

#include "OutputLog.hpp"

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

namespace m5emulator {

static bool writeAll(int descriptor, const char* data, std::size_t size) noexcept
{
	while (size)
	{
		const ssize_t count = write(descriptor, data, size);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) return false;
		data += count;
		size -= count;
	}
	return true;
}

OutputLog::OutputLog(int descriptor, const std::filesystem::path& path) : _descriptor(descriptor)
{
	try
	{
		_file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (_file < 0) throw std::system_error(errno, std::generic_category(), "create log " + path.string());
		_original = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
		if (_original < 0) throw std::system_error(errno, std::generic_category(), "preserve console output");
		if (pipe(_pipe.data()) < 0) throw std::system_error(errno, std::generic_category(), "log pipe");
		for (const int fd : _pipe)
		{
			if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) throw std::system_error(errno, std::generic_category(), "log close-on-exec");
		}
		
		_reader = std::thread(&OutputLog::forward, this);
		std::fflush(descriptor == STDOUT_FILENO ? stdout : stderr);
		if (dup2(_pipe[1], descriptor) < 0) throw std::system_error(errno, std::generic_category(), "redirect log output");
		::close(_pipe[1]);
		_pipe[1] = -1;
	}
	catch (...)
	{
		close();
		throw;
	}
}

OutputLog::~OutputLog() { close(); }

void OutputLog::forward() noexcept
{
	// A closed terminal pipe must not stop logging or terminate the simulator.
	sigset_t signals;
	sigemptyset(&signals);
	sigaddset(&signals, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &signals, nullptr);
	bool fileAvailable = true, consoleAvailable = true;
	char buffer[8192];
	while (true)
	{
		const ssize_t count = read(_pipe[0], buffer, sizeof(buffer));
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) break;
		if (fileAvailable && !writeAll(_file, buffer, count))
		{
			fileAvailable = false;
			static constexpr char Message[] = "M5 Emulator: writing the log failed; console output continues.\n";
			writeAll(_original, Message, sizeof(Message) - 1);
		}
		if (consoleAvailable) consoleAvailable = writeAll(_original, buffer, count);
	}
}

void OutputLog::close() noexcept
{
	// Release our pipe writers before joining the reader so it can reach EOF.
	if (_original >= 0)
	{
		std::fflush(_descriptor == STDOUT_FILENO ? stdout : stderr);
		int result;
		do { result = dup2(_original, _descriptor); } while (result < 0 && errno == EINTR);
		if (result < 0) ::close(_descriptor);
	}
	if (_pipe[1] >= 0) ::close(_pipe[1]);
	if (_reader.joinable()) _reader.join();
	for (const int fd : { _pipe[0], _file, _original })
	{
		if (fd >= 0) ::close(fd);
	}
}

} // namespace m5emulator
