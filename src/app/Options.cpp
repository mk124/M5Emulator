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

#include "Options.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <SDL.h>

#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif

#include "models/Device.hpp"
#include "Firmware.hpp"

namespace m5emulator {

namespace fs = std::filesystem;

static void printUsage(std::string_view executable)
{
	std::cout << "Usage: " << executable << " [options]\n\n"
	                                        "Run without --flash to open the window and drop a BIN.\n\n"
	                                        "  --flash FILE.bin        Load an application or full Flash image\n"
	                                        "  --device ID             Select hardware (default: stopwatch)\n"
	                                        "  --no-audio              Disable speaker and microphone\n"
	                                        "  --[no-]mic              Enable/disable microphone\n"
	                                        "  --[no-]bluetooth        Enable/disable host BLE (macOS)\n"
	                                        "  --[no-]persist          Enable/disable Flash persistence\n"
	                                        "  --[no-]icount           Enable/disable instruction-count timing\n"
	                                        "  --[no-]brightness       Enable/disable panel brightness simulation\n"
	                                        "  --state-flash FILE      Use a specific Flash save\n"
	                                        "  --capture-dir DIR       Set screenshot and recording directory\n"
	                                        "  --record FILE.mp4       Record video and audio (macOS)\n"
	                                        "  --screenshot FILE.bmp   Save a screenshot on exit\n"
	                                        "  --headless              Run without a window\n"
	                                        "  --seconds N             Exit after N seconds\n"
	                                        "  --qemu PATH             Use a specific QEMU executable\n"
	                                        "  --ble-peer SOCKET       Use an external BLE protocol peer\n"
	                                        "  --[no-]ble-hle          Enable/disable controller interception\n"
	                                        "  -h, --help              Show this help\n";
}

static double parseDurationSeconds(std::string_view value)
{
	const std::string text(value);
	const locale_t locale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
	if (locale == nullptr) throw std::runtime_error("Cannot create numeric locale");
	
	char* end = nullptr;
	const double seconds = strtod_l(text.c_str(), &end, locale);
	freelocale(locale);
	
	if (end != text.c_str() + text.size() || value.starts_with('+') ||
	    value.find_first_not_of("0123456789.eE+-") != std::string_view::npos ||
	    !std::isfinite(seconds) || seconds <= 0)
	{
		throw std::runtime_error("--seconds must be a finite positive number");
	}
	return seconds;
}

void validateOptions(const Options& options)
{
	findDevice(options.deviceId);
	if (!options.controllerHle && (options.bluetooth || !options.blePeerSocket.empty())) throw std::runtime_error("BLE requires controller HLE");
	if (options.bluetooth && !options.blePeerSocket.empty()) throw std::runtime_error("--bluetooth cannot be combined with --ble-peer");
	#ifndef __APPLE__
	if (options.bluetooth) throw std::runtime_error("--bluetooth requires macOS CoreBluetooth");
	#endif
	if (options.microphone && !options.audio) throw std::runtime_error("--mic cannot be combined with --no-audio");
	if (options.flashPath.empty())
	{
		if (options.headless) throw std::runtime_error("--headless requires --flash");
		if (!options.stateFlashPath.empty()) throw std::runtime_error("--state-flash requires --flash");
		return;
	}
	validateFirmware(options.flashPath, findDevice(options.deviceId).flash);
	if (!options.stateFlashPath.empty() && fs::exists(options.stateFlashPath))
	{
		if (fs::equivalent(options.flashPath, options.stateFlashPath))
		{
			throw std::runtime_error("Persistent Flash must be separate from the source BIN");
		}
	}
	
	if (!options.screenshotPath.empty() && fs::exists(options.screenshotPath) &&
	    fs::equivalent(options.flashPath, options.screenshotPath))
	{
		throw std::runtime_error("Screenshot must not overwrite the source flash");
	}
	if (!options.screenshotPath.empty() && !options.stateFlashPath.empty() &&
	    (fs::weakly_canonical(fs::absolute(options.screenshotPath)) == fs::weakly_canonical(fs::absolute(options.stateFlashPath)) ||
	     (fs::exists(options.screenshotPath) && fs::exists(options.stateFlashPath) &&
	      fs::equivalent(options.screenshotPath, options.stateFlashPath))))
	{
		throw std::runtime_error("Screenshot must not overwrite persistent Flash");
	}
}

std::optional<Options> parseOptions(int argc, char** argv)
{
	Options options;
	#ifdef __APPLE__
	const std::unique_ptr<char, decltype(&SDL_free)> directory(SDL_GetBasePath(), SDL_free);
	if (!directory) throw std::runtime_error("Cannot locate application resources: " + std::string(SDL_GetError()));
	const fs::path resources(directory.get());
	#if defined(__aarch64__)
	const fs::path bundledQemu = resources / "../Helpers/arm64/qemu-system-xtensa";
	#elif defined(__x86_64__)
	const fs::path bundledQemu = resources / "../Helpers/x86_64/qemu-system-xtensa";
	#endif
	if (fs::exists(bundledQemu))
	{
		options.qemuPath = bundledQemu.lexically_normal().string();
		options.qemuBiosPath = (resources / "qemu").string();
	}
	#endif
	
	bool explicitMicrophone = false, explicitBluetooth = false;
	for (int i = 1; i < argc; ++i)
	{
		const std::string_view arg = argv[i];
		if (arg == "--help" || arg == "-h")
		{
			printUsage(argv[0]);
			return std::nullopt;
		}
		if (arg == "--headless")
		{
			options.headless = true;
			continue;
		}
		if (arg == "--ble-hle" || arg == "--no-ble-hle")
		{
			options.controllerHle = arg == "--ble-hle";
			continue;
		}
		if (arg == "--icount" || arg == "--no-icount")
		{
			options.icount = arg == "--icount";
			continue;
		}
		if (arg == "--brightness" || arg == "--no-brightness")
		{
			options.simulateBrightness = arg == "--brightness";
			continue;
		}
		if (arg == "--persist" || arg == "--no-persist")
		{
			options.persistFlash = arg == "--persist";
			continue;
		}
		if (arg == "--bluetooth" || arg == "--no-bluetooth")
		{
			options.bluetooth = arg == "--bluetooth";
			explicitBluetooth = options.bluetooth;
			continue;
		}
		if (arg == "--mic" || arg == "--no-mic")
		{
			options.microphone = arg == "--mic";
			explicitMicrophone = options.microphone;
			continue;
		}
		if (arg == "--no-audio")
		{
			options.audio = false;
			continue;
		}
		
		if (arg != "--device" && arg != "--flash" && arg != "--qemu" && arg != "--seconds" &&
		    arg != "--screenshot" && arg != "--ble-peer" && arg != "--state-flash" && arg != "--capture-dir" && arg != "--record")
		{
			throw std::runtime_error("Unknown option: " + std::string(arg));
		}
		if (++i == argc || std::string_view(argv[i]).empty() ||
		    std::string_view(argv[i]).starts_with("--"))
		{
			throw std::runtime_error("Missing value for " + std::string(arg));
		}
		
		const std::string_view value = argv[i];
		if (arg == "--device") options.deviceId = value;
		else if (arg == "--flash") options.flashPath = value;
		else if (arg == "--state-flash") options.stateFlashPath = value;
		else if (arg == "--qemu") options.qemuPath = value;
		else if (arg == "--screenshot") options.screenshotPath = value;
		else if (arg == "--record") options.recordPath = value;
		else if (arg == "--capture-dir") options.captureDirectory = value;
		else if (arg == "--ble-peer") options.blePeerSocket = value;
		else if (arg == "--seconds") options.durationSeconds = parseDurationSeconds(value);
	}
	
	if (!options.audio && !explicitMicrophone) options.microphone = false;
	if ((!options.blePeerSocket.empty() || !options.controllerHle) && !explicitBluetooth) options.bluetooth = false;
	if (!options.persistFlash && !options.stateFlashPath.empty()) throw std::runtime_error("--no-persist cannot be combined with --state-flash");
	if (!options.recordPath.empty() && options.flashPath.empty()) throw std::runtime_error("--record requires --flash");
	validateOptions(options);
	
	return options;
}

} // namespace m5emulator
