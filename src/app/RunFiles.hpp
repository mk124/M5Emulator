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

#include <filesystem>
#include <string>

#include "models/Device.hpp"

namespace m5emulator {

class RunFiles
{
public:
	RunFiles(const Device& device, const std::filesystem::path& sourceFlash, const std::filesystem::path& stateFlash, bool replaceState = false);
	~RunFiles();
	
	RunFiles(const RunFiles&) = delete;
	RunFiles& operator=(const RunFiles&) = delete;
	
	void resetState();
	// Publish input fields and read the complete snapshot under one lock.
	void exchange(DeviceState& snapshot);
	
private:
	RunFiles(const Device& device);
	
public:
	const std::filesystem::path& flashPath() const { return _flashPath; }
	std::filesystem::path sharedPath() const { return std::filesystem::path(_directory) / "state.bin"; }
	
private:
	const Device& _device;
	std::string _directory = (std::filesystem::absolute(std::filesystem::temp_directory_path()) / "m5-emulator-XXXXXX").string();
	
	std::filesystem::path _flashPath;
	int _flashLockFd = -1;
	
	int _sharedFd = -1;
	void* _shared = nullptr;
};

} // namespace m5emulator
