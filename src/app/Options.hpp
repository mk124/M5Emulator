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
#include <optional>
#include <string>

#include "models/Device.hpp"

namespace m5emulator {

struct Options
{
	std::string deviceId { Devices.front().id };
	
	std::filesystem::path flashPath;
	std::filesystem::path stateFlashPath;
	bool persistFlash = true;
	
	std::string qemuPath = M5EMU_DEFAULT_QEMU;
	std::string qemuBiosPath = M5EMU_QEMU_BIOS;
	bool audio = true, microphone = true;
	bool icount = false;
	
	bool controllerHle = true;
	#ifdef __APPLE__
	bool bluetooth = true;
	#else
	bool bluetooth = false;
	#endif
	std::filesystem::path blePeerSocket;
	
	std::optional<double> durationSeconds;
	bool headless = false;
	bool simulateBrightness = false;
	std::filesystem::path screenshotPath;
	std::filesystem::path recordPath;
	std::filesystem::path captureDirectory;
	
	bool usesBluetooth() const { return bluetooth && controllerHle; }
};

void validateOptions(const Options& options);
std::optional<Options> parseOptions(int argc, char** argv);

} // namespace m5emulator
