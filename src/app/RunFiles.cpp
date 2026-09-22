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

#include "RunFiles.hpp"

#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>

#include "FileLock.hpp"
#include "Firmware.hpp"

namespace m5emulator {

namespace fs = std::filesystem;

static void replaceFlash(const fs::path& source, const fs::path& destination, const FlashLayout& layout)
{
	std::string temporary = destination.string() + ".tmp-XXXXXX";
	const int fd = mkstemp(temporary.data());
	if (fd < 0) throw std::system_error(errno, std::generic_category(), "create Flash staging file");
	close(fd);
	try
	{
		writeFlash(source, temporary, layout);
		fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
		fs::rename(temporary, destination);
	}
	catch (...)
	{
		std::error_code ignored;
		fs::remove(temporary, ignored);
		throw;
	}
}

// Delegation establishes directory ownership before fallible file setup.
RunFiles::RunFiles(const Device& device, const fs::path& sourceFlash, const fs::path& stateFlash, bool replaceState) : RunFiles(device)
{
	_sharedFd = open(sharedPath().c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (_sharedFd < 0) throw std::system_error(errno, std::generic_category(), "open shared state");
	if (ftruncate(_sharedFd, _device.sharedSize) < 0)
	{
		throw std::system_error(errno, std::generic_category(), "size shared state");
	}
	
	void* mapping = mmap(nullptr, _device.sharedSize, PROT_READ | PROT_WRITE, MAP_SHARED, _sharedFd, 0);
	if (mapping == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "mmap");
	_shared = mapping;
	resetState();
	
	_flashPath = stateFlash.empty() ? fs::path(_directory) / "flash.bin" : fs::weakly_canonical(fs::absolute(stateFlash));
	if (!stateFlash.empty())
	{
		fs::create_directories(_flashPath.parent_path());
		const fs::path lockPath = _flashPath.string() + ".lock";
		_flashLockFd = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (_flashLockFd < 0) throw std::system_error(errno, std::generic_category(), "open persistent Flash lock");
		if (flock(_flashLockFd, LOCK_EX | LOCK_NB) < 0)
		{
			throw std::system_error(errno, std::generic_category(), "persistent Flash is already in use: " + _flashPath.string());
		}
		if (replaceState || !fs::exists(_flashPath)) replaceFlash(sourceFlash, _flashPath, _device.flash);
		else validateFlash(_flashPath, _device.flash);
	}
	else
	{
		writeFlash(sourceFlash, _flashPath, _device.flash);
		fs::permissions(_flashPath, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
	}
}

RunFiles::~RunFiles()
{
	if (_shared != nullptr) munmap(_shared, _device.sharedSize);
	if (_sharedFd >= 0) close(_sharedFd);
	if (_flashLockFd >= 0) close(_flashLockFd);
	
	std::error_code ignored;
	fs::remove_all(_directory, ignored);
}

void RunFiles::resetState()
{
	const FileLock lock(_sharedFd);
	_device.resetShared(_shared);
}

void RunFiles::exchange(DeviceState& snapshot)
{
	const FileLock lock(_sharedFd);
	_device.exchangeShared(_shared, snapshot);
}

RunFiles::RunFiles(const Device& device) : _device(device)
{
	if (mkdtemp(_directory.data()) == nullptr) throw std::system_error(errno, std::generic_category(), "mkdtemp");
}

} // namespace m5emulator
