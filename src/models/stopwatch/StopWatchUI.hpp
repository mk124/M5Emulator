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

#include <array>
#include <chrono>

#include "StopWatchLayout.hpp"

#include "ui/DeviceUI.hpp"
#include "ui/EmulatorControls.hpp"
#include "ui/Input.hpp"

namespace m5emulator {

class StopWatchUI final : public DeviceUI
{
	static constexpr int BorderWidth = 3, BorderSegments = 128;
	
public:
	StopWatchUI();
	void reset() override;
	void draw(Display& display, const DeviceFeedback& feedback, const Options& options, std::span<const std::filesystem::path> recentFiles, bool recording) override;
	void resetInput() override;
	UiAction handleEvent(const SDL_Event& event, const Options& options, std::span<const std::filesystem::path> recentFiles, bool running) override;
	void setNotice(std::string_view message, bool error = false) override;
	
private:
	void drawIdle(Display& display);
	void drawControls(Display& display, const DeviceFeedback& feedback);
	
public:
	SDL_Point windowSize() const override { return { _layout.width, _layout.height }; }
	InputState sampleInput(float elapsedSeconds) override { return _input.sample(elapsedSeconds); }
	
private:
	StopWatchLayout _layout;
	EmulatorControls _controls;
	Input _input;
	std::array<std::array<SDL_FPoint, BorderSegments + 1>, BorderWidth> _screenBorders {};
	uint16_t _vibrationLevel = 0;
	std::chrono::steady_clock::time_point _vibrationUntil {};
};

} // namespace m5emulator
