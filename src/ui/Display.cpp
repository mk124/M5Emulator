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

#include "Display.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace m5emulator {

void Display::checkSdl(bool success, const char* action)
{
	if (!success) throw std::runtime_error(std::string(action) + ": " + SDL_GetError());
}

Display::Display(const Options& options) : _session(options.headless), _ui(DeviceUI::create(options.deviceId))
{
	const SDL_Point size = _ui->windowSize();
	if (!options.headless)
	{
		_window.reset(SDL_CreateWindow("M5 Emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, size.x, size.y, SDL_WINDOW_ALLOW_HIGHDPI));
		checkSdl(_window != nullptr, "create window");
		_renderer.reset(SDL_CreateRenderer(_window.get(), -1, SDL_RENDERER_ACCELERATED));
		if (!_renderer) _renderer.reset(SDL_CreateRenderer(_window.get(), -1, SDL_RENDERER_SOFTWARE));
		checkSdl(_renderer != nullptr, "create renderer");
		_verticalSync = SDL_RenderSetVSync(_renderer.get(), 1) == 0;
	}
	setDevice(options.deviceId);
	setFirmware(options.flashPath);
}

void Display::setDevice(std::string_view deviceId)
{
	auto next = DeviceUI::create(deviceId);
	const auto& device = findDevice(deviceId);
	const auto& screen = device.screen;
	const SDL_Point size = next->windowSize();
	std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> surface(SDL_CreateRGBSurfaceWithFormat(0, screen.width, screen.height, 16, SDL_PIXELFORMAT_RGB565), SDL_FreeSurface);
	checkSdl(surface != nullptr, "create display surface");
	checkSdl(SDL_FillRect(surface.get(), nullptr, 0) == 0, "clear display surface");
	Texture texture(nullptr, SDL_DestroyTexture);
	if (_renderer)
	{
		texture.reset(SDL_CreateTexture(_renderer.get(), SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, screen.width, screen.height));
		checkSdl(texture != nullptr, "create display texture");
		checkSdl(SDL_UpdateTexture(texture.get(), nullptr, surface->pixels, surface->pitch) == 0, "clear texture");
		checkSdl(SDL_RenderSetLogicalSize(_renderer.get(), size.x, size.y) == 0, "set display size");
		SDL_SetWindowSize(_window.get(), size.x, size.y);
	}
	
	_surface = std::move(surface);
	_texture = std::move(texture);
	_device = &device;
	_ui = std::move(next);
}

void Display::setFirmware(const std::filesystem::path& path)
{
	if (_window)
	{
		const std::string title = path.empty() ? "M5 Emulator" : std::string(_device->name) + " | " + path.filename().string();
		SDL_SetWindowTitle(_window.get(), title.c_str());
	}
	
	_ui->reset();
}

void Display::update(std::span<const uint16_t> pixels)
{
	const auto& screen = _device->screen;
	if (pixels.size() != screen.pixelCount()) throw std::runtime_error("Frame dimensions do not match the selected device");
	checkSdl(SDL_LockSurface(_surface.get()) == 0, "lock display surface");
	auto* surfaceBytes = static_cast<uint8_t*>(_surface->pixels);
	for (int y = 0; y < screen.height; ++y)
	{
		const uint16_t* sourceRow = pixels.data() + y * screen.width;
		auto* targetRow = reinterpret_cast<uint16_t*>(surfaceBytes + y * _surface->pitch);
		for (int x = 0; x < screen.width; ++x)
		{
			targetRow[x] = screen.contains(x, y) ? sourceRow[x] : 0;
		}
	}
	SDL_UnlockSurface(_surface.get());
	
	if (_texture)
	{
		checkSdl(SDL_UpdateTexture(_texture.get(), nullptr, _surface->pixels, _surface->pitch) == 0, "update display texture");
	}
}

void Display::draw(const DeviceFeedback& feedback, const Options& options, std::span<const std::filesystem::path> recentFiles, bool recording)
{
	if (!_renderer) return;
	_ui->draw(*this, feedback, options, recentFiles, recording);
	SDL_RenderPresent(_renderer.get());
}

void Display::saveScreenshot(const std::filesystem::path& path) const
{
	checkSdl(SDL_SaveBMP(_surface.get(), path.c_str()) == 0, "save screenshot");
}

void Display::drawScreen(const SDL_Rect& rect)
{
	checkSdl(SDL_RenderCopy(_renderer.get(), _texture.get(), nullptr, &rect) == 0, "render display");
}

void Display::drawTag(std::string_view text, const SDL_Rect& rect, int padding, int scale, SDL_Color background, SDL_Color foreground)
{
	SDL_SetRenderDrawColor(_renderer.get(), background.r, background.g, background.b, background.a);
	checkSdl(SDL_RenderFillRect(_renderer.get(), &rect) == 0, "render tag");
	drawText(text, rect.x + padding, rect.y + padding, scale, foreground);
}

} // namespace m5emulator
