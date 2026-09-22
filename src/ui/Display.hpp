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

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

#include <SDL.h>

#include "app/Options.hpp"
#include "models/Device.hpp"
#include "DeviceUI.hpp"
#include "SdlSession.hpp"

namespace m5emulator {

class Display
{
	using Texture = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;
	
public:
	static constexpr int GlyphWidth = 5, GlyphHeight = 7, GlyphAdvance = GlyphWidth + 1;
	static void checkSdl(bool success, const char* action);
	
	explicit Display(const Options& options);
	void setDevice(std::string_view deviceId);
	void setFirmware(const std::filesystem::path& path);
	void update(std::span<const uint16_t> pixels);
	void draw(const DeviceFeedback& feedback, const Options& options, std::span<const std::filesystem::path> recentFiles, bool recording);
	void saveScreenshot(const std::filesystem::path& path) const;
	
	void drawScreen(const SDL_Rect& rect);
	void drawText(std::string_view text, int x, int y, int scale, SDL_Color color);
	void drawTag(std::string_view text, const SDL_Rect& rect, int padding, int scale, SDL_Color background, SDL_Color foreground);
	
	[[nodiscard]] DeviceUI& ui() const { return *_ui; }
	[[nodiscard]] const Device& device() const { return *_device; }
	[[nodiscard]] SDL_Renderer* renderer() const { return _renderer.get(); }
	[[nodiscard]] bool syncsToDisplay() const { return _verticalSync && !(SDL_GetWindowFlags(_window.get()) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)); }
	
private:
	SdlSession _session;
	const Device* _device = nullptr;
	std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> _surface { nullptr, SDL_FreeSurface };
	std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> _window { nullptr, SDL_DestroyWindow };
	std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> _renderer { nullptr, SDL_DestroyRenderer };
	Texture _texture { nullptr, SDL_DestroyTexture };
	bool _verticalSync = false;
	// Device UI textures must be released before the renderer.
	std::unique_ptr<DeviceUI> _ui;
};

} // namespace m5emulator
