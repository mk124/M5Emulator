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

#include "Application.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace m5emulator {

static std::filesystem::path defaultCaptureDirectory()
{
	const char* userHome = std::getenv("HOME");
	if (userHome != nullptr && *userHome) return std::filesystem::path(userHome) / "Pictures" / "M5 Emulator";
	
	const std::unique_ptr<char, decltype(&SDL_free)> directory(SDL_GetPrefPath("", "M5Emulator"), SDL_free);
	if (!directory) throw std::runtime_error("Cannot locate capture storage: " + std::string(SDL_GetError()));
	return std::filesystem::path(directory.get()) / "captures";
}

static std::filesystem::path capturePath(const Options& options, const char* pattern)
{
	const std::filesystem::path directory = options.captureDirectory.empty() ? defaultCaptureDirectory() : options.captureDirectory;
	std::filesystem::create_directories(directory);
	
	const std::time_t now = std::time(nullptr);
	std::tm localTime {};
	std::array<char, 64> filename;
	if (localtime_r(&now, &localTime) == nullptr || std::strftime(filename.data(), filename.size(), pattern, &localTime) == 0) throw std::runtime_error("Cannot format capture timestamp");
	
	std::string path = (directory / filename.data()).string();
	const int fd = mkstemps(path.data(), 4);
	if (fd < 0) throw std::system_error(errno, std::generic_category(), "create capture");
	close(fd);
	return path;
}

void Application::captureScreen()
{
	if (!_files) return;
	if (_running) exchangeState();
	
	const auto path = capturePath(_options, "capture-%Y%m%d-%H%M%S-XXXXXX.bmp");
	try
	{
		_display.saveScreenshot(path);
	}
	catch (...)
	{
		std::error_code ignored;
		std::filesystem::remove(path, ignored);
		throw;
	}
	
	_display.ui().setNotice("CAPTURE SAVED " + path.filename().string());
	std::cout << "Screen captured: " << std::filesystem::absolute(path) << '\n'
	          << std::flush;
}

void Application::toggleRecording()
{
	if (_recorder.active())
	{
		stopRecording(false);
		return;
	}
	if (!_running) return;
	if (_recorder.finishing())
	{
		_display.ui().setNotice("SAVING RECORDING");
		return;
	}
	
	std::filesystem::path path = _options.recordPath;
	if (path.empty())
	{
		path = capturePath(_options, "recording-%Y%m%d-%H%M%S-XXXXXX.mp4");
		std::filesystem::remove(path); // AVAssetWriter creates a new file itself.
	}
	else if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
	
	_recordingFailed = false;
	_recorder.start(path, _options.audio, _display.device().screen);
	try
	{
		exchangeState();
		_recorder.appendFrame(_snapshot.pixels);
		if (_options.audio) _qemu.recordAudio(true, _recorder);
	}
	catch (...)
	{
		try
		{
			stopRecording();
		}
		catch (...)
		{}
		throw;
	}
	
	_options.recordPath.clear();
	_display.ui().setNotice("RECORDING STARTED / F9 TO STOP");
	std::cout << "Recording started: " << std::filesystem::absolute(path) << '\n'
	          << std::flush;
}

void Application::stopRecording(bool endingSession)
{
	if (!_recorder.active()) return;
	if (!_recordingStopNs)
	{
		_recordingStopNs = m5RecordingTimeNs();
		if (_running && _options.audio)
		{
			try
			{
				_qemu.recordAudio(false, _recorder);
			}
			catch (const std::exception& error)
			{
				std::cerr << "Recording audio: " << error.what() << '\n';
				endingSession = true;
			}
		}
		_display.ui().setNotice("SAVING RECORDING");
	}
	if (!endingSession && _running && _options.audio) return;
	
	// Restart/unload cannot wait on the old process; retain all audio already delivered.
	try
	{
		_qemu.drainRecordedAudio(_recorder);
	}
	catch (const std::exception& error)
	{
		std::cerr << "Recording audio: " << error.what() << '\n';
	}
	const uint64_t endNs = std::exchange(_recordingStopNs, 0);
	_recorder.finish(endNs);
}
	
void Application::pollRecording()
{
	try
	{
		if (_recordingStopNs)
		{
			const uint32_t status = _qemu.drainRecordedAudio(_recorder);
			const bool expired = m5RecordingTimeNs() - _recordingStopNs >= 2'000'000'000;
			if (status == M5RecordingStopped || status == M5RecordingFailed || !_running || expired)
			{
				if (expired || status == M5RecordingFailed) std::cerr << "Recording audio: stop acknowledgement failed; saving received samples\n";
				const uint64_t endNs = std::exchange(_recordingStopNs, 0);
				_recorder.finish(endNs);
			}
		}
		if (!_recorder.pollFinished()) return;
		_display.ui().setNotice("RECORDING SAVED " + _recorder.path().filename().string());
		std::cout << "Recording saved: " << std::filesystem::absolute(_recorder.path()) << '\n'
		          << std::flush;
	}
	catch (const std::exception& error)
	{
		_recordingFailed = true;
		stopRecording();
		reportError(error.what());
	}
}

} // namespace m5emulator
