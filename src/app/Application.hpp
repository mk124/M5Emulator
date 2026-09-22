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

#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

#include "models/DeviceState.hpp"

#include "ui/Display.hpp"
#include "ui/Setting.hpp"
#include "VideoRecorder.hpp"

#ifdef __APPLE__
#include "bluetooth/BluetoothBridge.hpp"
#endif

#include "Options.hpp"
#include "QemuProcess.hpp"
#include "RecentFiles.hpp"
#include "RunFiles.hpp"

namespace m5emulator {

class Application
{
	using Clock = std::chrono::steady_clock;
	
public:
	explicit Application(const Options& options);
	int run(const volatile std::sig_atomic_t& stopSignal);
	
private:
	void reportError(std::string_view message);
	
	void openFirmware(const Options& options);
	void openFirmware(const std::filesystem::path& path);
	void unloadFirmware();
	void restart();
	void changePersistence();
	void exchangeState();
	
	void captureScreen();
	void toggleRecording();
	void stopRecording(bool endingSession = true);
	void pollRecording();
	
	void handleEvent(const SDL_Event& event);
	void changeSetting(Setting setting);
	void selectDevice(std::size_t index);
	
	Options _options;
	Display _display;
	
	RecentFiles _recentFiles;
	
	std::unique_ptr<RunFiles> _files;
	#ifdef __APPLE__
	std::optional<bluetooth::BluetoothBridge> _bluetooth;
	#endif
	QemuProcess _qemu;
	VideoRecorder _recorder;
	uint64_t _recordingStopNs = 0;
	bool _recordingFailed = false;
	bool _running = false;
	
	bool _quit = false;
	
	DeviceState _snapshot {};
	uint64_t _lastUpdate = 0, _displayUpdates = 0;
	Clock::time_point _inputTime = Clock::now();
};

} // namespace m5emulator
