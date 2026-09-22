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

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

#include <sys/wait.h>

namespace m5emulator {

using Seconds = std::chrono::duration<double>;

static constexpr auto FrameInterval = std::chrono::nanoseconds(16'666'667);

Application::Application(const Options& options)
    : _options(options)
    , _display(options)
    , _recentFiles(options.deviceId)
{}

int Application::run(const volatile std::sig_atomic_t& stopSignal)
{
	if (!_options.flashPath.empty()) openFirmware(_options);
	if (!_options.recordPath.empty()) toggleRecording();
	
	const auto start = Clock::now();
	auto nextFrame = start;
	int exitCode = EXIT_SUCCESS;
	while (!_quit && !stopSignal)
	{
		#ifdef __APPLE__
		if (_bluetooth) _bluetooth->poll();
		#endif
		
		int status = 0;
		if (_running && _qemu.exited(status))
		{
			_running = false;
			stopRecording();
			_qemu.stop();
			#ifdef __APPLE__
			if (_bluetooth) _bluetooth->reset();
			#endif
			
			std::string message = "QEMU stopped";
			if (WIFEXITED(status)) message += " (exit " + std::to_string(WEXITSTATUS(status)) + ')';
			else if (WIFSIGNALED(status)) message += " (signal " + std::to_string(WTERMSIG(status)) + ')';
			reportError(message + ". Press F5 to retry or drop another BIN.");
			if (_options.headless)
			{
				exitCode = EXIT_FAILURE;
				break;
			}
		}
		if (_options.durationSeconds && Seconds(Clock::now() - start).count() >= *_options.durationSeconds) break;
		
		SDL_Event event;
		while (!_options.headless && !_quit && SDL_PollEvent(&event)) handleEvent(event);
		if (_running) exchangeState();
		
		pollRecording();
		if (_recorder.active() && !_recordingStopNs)
		{
			try
			{
				_qemu.drainRecordedAudio(_recorder);
				_recorder.appendFrame(_snapshot.pixels);
			}
			catch (const std::exception& error)
			{
				const std::string message = error.what();
				try
				{
					stopRecording();
				}
				catch (const std::exception& stopError)
				{
					std::cerr << stopError.what() << '\n';
				}
				reportError(message);
			}
		}
		
		_display.draw(_snapshot.feedback, _options, _recentFiles.entries(), _recorder.active() && !_recordingStopNs);
		
		// VSync paces visible windows; other modes retain the existing timer.
		if (!_display.syncsToDisplay())
		{
			nextFrame = std::max(nextFrame + FrameInterval, Clock::now());
			double delaySeconds = Seconds(nextFrame - Clock::now()).count();
			if (_options.durationSeconds) delaySeconds = std::min(delaySeconds, *_options.durationSeconds - Seconds(Clock::now() - start).count());
			if (delaySeconds > 0 && !_quit && !stopSignal) std::this_thread::sleep_for(Seconds(delaySeconds));
		}
	}
	
	int status = 0;
	if (_running && _qemu.exited(status)) exitCode = EXIT_FAILURE;
	stopRecording(false);
	while (_recordingStopNs || _recorder.finishing())
	{
		if (_running && _qemu.exited(status)) _running = false;
		pollRecording();
		#ifdef __APPLE__
		if (_bluetooth) _bluetooth->poll();
		#endif
		SDL_PumpEvents();
		_display.draw(_snapshot.feedback, _options, _recentFiles.entries(), false);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	
	_qemu.stop();
	_running = false;
	if (_files) exchangeState();
	
	if (!_options.screenshotPath.empty()) _display.saveScreenshot(_options.screenshotPath);
	if (_files)
	{
		std::cout << std::fixed << std::setprecision(3)
		          << "wall duration: " << Seconds(Clock::now() - start).count() << " s\n"
		          << "guest_time_ns: " << _snapshot.feedback.guestTimeNs << '\n'
		          << "completed GRAM window writes: " << _snapshot.completedWindows << '\n'
		          << "TE intervals with completed writes: " << _snapshot.feedback.activeRefreshes << '\n'
		          << "display updates (observed changed snapshots): " << _displayUpdates << '\n';
	}
	
	return stopSignal ? 128 + stopSignal : _recordingFailed ? EXIT_FAILURE
	                                                        : exitCode;
}

void Application::reportError(std::string_view message)
{
	std::cerr << "Error: " << message << '\n';
	_display.ui().setNotice(message, true);
}

} // namespace m5emulator
