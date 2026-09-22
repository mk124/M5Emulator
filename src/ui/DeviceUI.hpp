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
#include <memory>
#include <span>
#include <string_view>

#include <SDL_events.h>
#include <SDL_rect.h>

#include "app/Options.hpp"
#include "models/DeviceFeedback.hpp"
#include "models/InputState.hpp"
#include "UiAction.hpp"

namespace m5emulator {

class Display;

class DeviceUI
{
public:
	static std::unique_ptr<DeviceUI> create(std::string_view deviceId);
	
	virtual ~DeviceUI() = default;
	virtual SDL_Point windowSize() const = 0;
	virtual void draw(Display& display, const DeviceFeedback& feedback, const Options& options, std::span<const std::filesystem::path> recentFiles, bool recording) = 0;
	virtual UiAction handleEvent(const SDL_Event& event, const Options& options, std::span<const std::filesystem::path> recentFiles, bool running) = 0;
	virtual InputState sampleInput(float elapsedSeconds) = 0;
	virtual void resetInput() = 0;
	virtual void reset() = 0;
	virtual void setNotice(std::string_view message, bool error = false) = 0;
};

} // namespace m5emulator
