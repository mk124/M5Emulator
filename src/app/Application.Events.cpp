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
#include <cctype>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace m5emulator {

void Application::handleEvent(const SDL_Event& event)
{
	try
	{
		if (event.type == SDL_QUIT)
		{
			_quit = true;
			return;
		}
		
		if (event.type == SDL_DROPFILE)
		{
			_display.ui().resetInput();
			const std::unique_ptr<char, decltype(&SDL_free)> filename(event.drop.file, SDL_free);
			if (!filename) return;
			const std::filesystem::path path(filename.get());
			std::string extension = path.extension().string();
			std::ranges::transform(extension, extension.begin(), [] (unsigned char character) { return std::tolower(character); });
			if (extension != ".bin") throw std::runtime_error("Drop an ESP32-S3 application .bin or a complete Flash .bin file.");
			openFirmware(path);
			return;
		}
		if (event.type == SDL_DROPTEXT)
		{
			SDL_free(event.drop.file);
			return;
		}
		
		const UiAction action = _display.ui().handleEvent(event, _options, _recentFiles.entries(), _running);
		switch (action.type)
		{
			case UiAction::Type::None:    break;
			case UiAction::Type::Quit:    _quit = true; break;
			case UiAction::Type::Restart: {
				restart();
				if (_files) std::cout << "Simulator restarted; working Flash preserved.\n"
					                  << std::flush;
				break;
			}
			case UiAction::Type::OpenRecent: {
				const auto& files = _recentFiles.entries();
				if (action.index < files.size()) openFirmware(files[action.index]);
				break;
			}
			case UiAction::Type::Eject:         unloadFirmware(); break;
			case UiAction::Type::Capture:       captureScreen(); break;
			case UiAction::Type::Record:        toggleRecording(); break;
			case UiAction::Type::SelectDevice:  selectDevice(action.index); break;
			case UiAction::Type::ChangeSetting: changeSetting(static_cast<Setting>(action.index)); break;
		}
	}
	catch (const std::exception& error)
	{
		reportError(error.what());
	}
}

void Application::changeSetting(Setting setting)
{
	switch (setting)
	{
		case Setting::Sound: {
			_options.audio = !_options.audio;
			if (!_options.audio) _options.microphone = false;
			break;
		}
		case Setting::Microphone: {
			_options.microphone = !_options.microphone;
			if (_options.microphone) _options.audio = true;
			break;
		}
		case Setting::Bluetooth: {
			#ifdef __APPLE__
			if (!_options.blePeerSocket.empty()) throw std::runtime_error("BLE is using the external peer selected on the command line.");
			_options.bluetooth = !_options.bluetooth;
			if (_options.bluetooth) _options.controllerHle = true;
			if (_running && _bluetooth)
			{
				_bluetooth->setEnabled(_options.bluetooth);
				_display.ui().setNotice({});
				return;
			}
			#else
			throw std::runtime_error("BLE requires macOS CoreBluetooth.");
			#endif
			break;
		}
		case Setting::Persistence: {
			changePersistence();
			return;
		}
		case Setting::Icount: {
			_options.icount = !_options.icount;
			break;
		}
		case Setting::Brightness: {
			_options.simulateBrightness = !_options.simulateBrightness;
			_display.ui().setNotice({});
			return;
		}
	}
	
	_display.ui().setNotice({});
	if (_files) restart();
}

void Application::selectDevice(std::size_t index)
{
	const auto& device = Devices[index];
	if (_options.deviceId == device.id) return;
	
	RecentFiles recent(device.id);
	unloadFirmware();
	_display.setDevice(device.id);
	_options.deviceId = device.id;
	_options.stateFlashPath.clear();
	_recentFiles = std::move(recent);
	_display.setFirmware({});
}

} // namespace m5emulator
