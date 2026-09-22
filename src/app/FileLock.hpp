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

#include <cerrno>
#include <system_error>

#include <sys/file.h>

namespace m5emulator {

class FileLock
{
public:
	FileLock(const FileLock&) = delete;
	FileLock& operator=(const FileLock&) = delete;
	
	explicit FileLock(int fd) : _fd(fd)
	{
		while (flock(_fd, LOCK_EX) < 0)
		{
			if (errno != EINTR) throw std::system_error(errno, std::generic_category(), "flock");
		}
	}
	
	~FileLock()
	{
		while (flock(_fd, LOCK_UN) < 0 && errno == EINTR) {}
	}
	
private:
	int _fd;
};

} // namespace m5emulator
