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

#include "Test.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

#include <SDL.h>
#include <unistd.h>

#include "ui/Display.hpp"

namespace m5emulator {

static void screenshotsSurviveRejectedDevicesAndReset(const std::filesystem::path& path)
{
	Options options;
	options.headless = true;
	Display display(options);
	const auto& stopwatch = findDevice("stopwatch");
	std::vector<uint16_t> pixels(466 * 466, 0xF800);
	display.update(pixels);
	display.saveScreenshot(path);
	
	const auto pixel = [] (const SDL_Surface& surface, int x, int y) {
		const auto* row = static_cast<const uint8_t*>(surface.pixels) + y * surface.pitch;
		uint32_t value = 0;
		std::memcpy(&value, row + x * surface.format->BytesPerPixel, surface.format->BytesPerPixel);
		uint8_t red, green, blue;
		SDL_GetRGB(value, surface.format, &red, &green, &blue);
		return (uint32_t(red) << 16) | (uint32_t(green) << 8) | blue;
	};
	std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> image(SDL_LoadBMP(path.c_str()), SDL_FreeSurface);
	CHECK(image != nullptr);
	CHECK(image->w == 466 && image->h == 466);
	CHECK(pixel(*image, 0, 0) == 0);
	CHECK(pixel(*image, 233, 233) == 0xFF0000);
	
	bool rejected = false;
	try
	{
		display.setDevice("unimplemented");
	}
	catch (const std::runtime_error&)
	{
		rejected = true;
	}
	CHECK(rejected);
	display.saveScreenshot(path);
	image.reset(SDL_LoadBMP(path.c_str()));
	CHECK(image != nullptr);
	CHECK(image->w == 466 && image->h == 466);
	CHECK(pixel(*image, 233, 233) == 0xFF0000);
	
	display.setDevice(stopwatch.id);
	display.saveScreenshot(path);
	image.reset(SDL_LoadBMP(path.c_str()));
	CHECK(image != nullptr);
	CHECK(image->w == 466 && image->h == 466);
	CHECK(pixel(*image, 233, 233) == 0);
	std::cout << "PASS StopWatch screenshot, rejected device and reset\n";
}

static void menusOwnKeyboardInputAndReleaseHeldButtons(const std::filesystem::path& file)
{
	CHECK(SDL_setenv("SDL_VIDEODRIVER", "dummy", 1) == 0);
	Options options;
	Display display(options);
	DeviceUI& ui = display.ui();
	const std::array files { file, file.parent_path() / "missing.bin" };
	const auto key = [&] (SDL_Keycode code) {
		SDL_Event event {};
		event.type = SDL_KEYDOWN;
		event.key.keysym.sym = code;
		return ui.handleEvent(event, options, files, true);
	};
	using Type = UiAction::Type;
	
	key(SDLK_1);
	CHECK(ui.sampleInput(0).buttons == 1);
	key(SDLK_F6);
	display.draw({}, options, files, false);
	CHECK(ui.sampleInput(0).buttons == 0);
	key(SDLK_2);
	CHECK(ui.sampleInput(0).buttons == 0);
	CHECK(key(SDLK_ESCAPE).type == Type::None);
	CHECK(key(SDLK_ESCAPE).type == Type::Quit);
	
	key(SDLK_F6);
	display.draw({}, options, files, false);
	key(SDLK_DOWN);
	CHECK(key(SDLK_RETURN).type == Type::None); // Missing files do not close the menu.
	key(SDLK_UP);
	const UiAction open = key(SDLK_RETURN);
	CHECK(open.type == Type::OpenRecent && open.index == 0);
	CHECK(key(SDLK_ESCAPE).type == Type::Quit);
	
	key(SDLK_F6);
	key(SDLK_UP); // Wrap to the final EJT row.
	CHECK(key(SDLK_RETURN).type == Type::Eject);
	key(SDLK_2);
	CHECK(ui.sampleInput(0).buttons == 2);
	key(SDLK_F4);
	display.draw({}, options, files, false);
	CHECK(ui.sampleInput(0).buttons == 0);
	key(SDLK_1);
	CHECK(ui.sampleInput(0).buttons == 0);
	CHECK(key(SDLK_ESCAPE).type == Type::None);
	CHECK(key(SDLK_ESCAPE).type == Type::Quit);
	
	key(SDLK_F6);
	CHECK(key(SDLK_F8).type == Type::Capture);
	CHECK(key(SDLK_ESCAPE).type == Type::Quit);
	std::cout << "PASS menus consume input, release buttons and dispatch file actions\n";
}

} // namespace m5emulator

int main()
{
	const auto pattern = (std::filesystem::temp_directory_path() / "m5-display-test-XXXXXX").string();
	std::vector<char> directory(pattern.begin(), pattern.end());
	directory.push_back('\0');
	if (mkdtemp(directory.data()) == nullptr) return 1;
	int result = 0;
	try
	{
		const auto file = std::filesystem::path(directory.data()) / "screen.bmp";
		m5emulator::screenshotsSurviveRejectedDevicesAndReset(file);
		m5emulator::menusOwnKeyboardInputAndReleaseHeldButtons(file);
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		result = 1;
	}
	std::filesystem::remove_all(directory.data());
	return result;
}
