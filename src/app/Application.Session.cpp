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
#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "Firmware.hpp"

namespace m5emulator {

static std::filesystem::path defaultStateFlashPath(const std::filesystem::path& source, std::string_view deviceId)
{
	const std::unique_ptr<char, decltype(&SDL_free)> directory(SDL_GetPrefPath("", "M5Emulator"), SDL_free);
	if (!directory) throw std::runtime_error("Cannot locate Flash storage: " + std::string(SDL_GetError()));
	const auto& device = findDevice(deviceId);
	return std::filesystem::path(directory.get()) / "devices" / device.id / "flash" / (flashDigest(source, device.flash) + ".bin");
}

void Application::openFirmware(const Options& options)
{
	Options selected = options;
	validateOptions(selected);
	if (selected.persistFlash && selected.stateFlashPath.empty())
	{
		selected.stateFlashPath = defaultStateFlashPath(selected.flashPath, selected.deviceId);
		validateOptions(selected);
	}
	
	const bool reuseFlash = _files && selected.persistFlash && std::filesystem::exists(selected.stateFlashPath) &&
	                        std::filesystem::equivalent(_files->flashPath(), selected.stateFlashPath);
	std::unique_ptr<RunFiles> files;
	if (!reuseFlash) files = std::make_unique<RunFiles>(findDevice(selected.deviceId), selected.flashPath, selected.persistFlash ? selected.stateFlashPath : std::filesystem::path {});
	
	// Prepare the new Flash before ending the current session; invalid drops keep it running.
	stopRecording();
	_qemu.stop();
	_running = false;
	#ifdef __APPLE__
	_bluetooth.reset();
	#endif
	if (files) _files = std::move(files);
	_options = std::move(selected);
	restart();
	
	try
	{
		_recentFiles.remember(_options.flashPath);
	}
	catch (const std::exception& error)
	{
		std::cerr << "Recent files: " << error.what() << '\n';
	}
	
	std::cout << "Firmware opened: " << _options.flashPath << '\n'
	          << (_options.persistFlash ? "Persistent Flash: " : "Temporary Flash: ") << _files->flashPath() << '\n'
	          << std::flush;
}

void Application::openFirmware(const std::filesystem::path& path)
{
	Options selected = _options;
	selected.flashPath = path;
	// Each GUI selection resolves its own save file.
	selected.stateFlashPath.clear();
	openFirmware(selected);
}

void Application::unloadFirmware()
{
	_display.ui().resetInput();
	if (!_files) return;
	
	stopRecording();
	_qemu.stop();
	_running = false;
	#ifdef __APPLE__
	_bluetooth.reset();
	#endif
	_files.reset();
	_options.flashPath.clear();
	_options.stateFlashPath.clear();
	
	_snapshot = {};
	_snapshot.pixels.resize(_display.device().screen.pixelCount());
	_lastUpdate = 0;
	_display.setFirmware({});
	_display.update(_snapshot.pixels);
	std::cout << "Firmware unloaded.\n"
	          << std::flush;
}

void Application::restart()
{
	_display.ui().resetInput();
	if (!_files) return;
	
	stopRecording();
	_qemu.stop();
	_running = false;
	
	Options active = _options;
	#ifdef __APPLE__
	if (_options.usesBluetooth() && !_bluetooth) _bluetooth.emplace(_files->sharedPath().parent_path() / "ble.sock");
	if (_bluetooth)
	{
		_bluetooth->setEnabled(_options.usesBluetooth());
		_bluetooth->reset();
		if (_options.controllerHle) active.blePeerSocket = _files->sharedPath().parent_path() / "ble.sock";
	}
	#endif
	
	_files->resetState();
	_snapshot = {};
	_snapshot.pixels.resize(_display.device().screen.pixelCount());
	_lastUpdate = 0;
	_display.setFirmware(_options.flashPath);
	_display.update(_snapshot.pixels);
	_qemu.start(active, _files->flashPath(), _files->sharedPath());
	_running = true;
	_inputTime = Clock::now();
}

void Application::changePersistence()
{
	if (!_files)
	{
		_options.persistFlash = !_options.persistFlash;
		_display.ui().setNotice({});
		return;
	}
	
	Options selected = _options;
	selected.persistFlash = !selected.persistFlash;
	validateOptions(selected);
	if (selected.persistFlash && selected.stateFlashPath.empty())
	{
		selected.stateFlashPath = defaultStateFlashPath(selected.flashPath, selected.deviceId);
		validateOptions(selected);
	}
	
	stopRecording();
	_qemu.stop();
	_running = false;
	std::unique_ptr<RunFiles> files;
	try
	{
		files = std::make_unique<RunFiles>(findDevice(selected.deviceId), _files->flashPath(), selected.persistFlash ? selected.stateFlashPath : std::filesystem::path {}, selected.persistFlash);
	}
	catch (...)
	{
		const auto error = std::current_exception();
		restart();
		std::rethrow_exception(error);
	}
	#ifdef __APPLE__
	_bluetooth.reset();
	#endif
	
	_files = std::move(files);
	_options = std::move(selected);
	restart();
	_display.ui().setNotice(_options.persistFlash ? "PERSISTENT FLASH ENABLED / DEVICE RESTARTED" : "TEMPORARY FLASH / SAVED DATA KEPT");
	std::cout << (_options.persistFlash ? "Persistent Flash: " : "Temporary Flash: ") << _files->flashPath() << '\n'
	          << std::flush;
}

void Application::exchangeState()
{
	const auto now = Clock::now();
	_snapshot.input = _display.ui().sampleInput(std::chrono::duration<float>(now - _inputTime).count());
	_inputTime = now;
	_snapshot.simulateBrightness = _options.simulateBrightness;
	_files->exchange(_snapshot);
	if (_snapshot.updates == _lastUpdate) return;
	
	_display.update(_snapshot.pixels);
	_lastUpdate = _snapshot.updates;
	++_displayUpdates;
}

} // namespace m5emulator
