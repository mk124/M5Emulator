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

#include "StopWatchUI.hpp"

#include <cmath>
#include <numbers>

#include "ui/Display.hpp"

namespace m5emulator {

static constexpr SDL_Color BackgroundColor { 0, 0, 0, 255 };
static constexpr SDL_Color ScreenBorderColor { 122, 139, 158, 255 };

static constexpr SDL_Color LabelColor { 215, 222, 232, 255 };
static constexpr SDL_Color MutedColor { 133, 147, 165, 255 };
static constexpr SDL_Color TrackColor { 38, 43, 50, 255 };
static constexpr SDL_Color PressedColor { 38, 120, 160, 255 };
static constexpr SDL_Color LedColor { 68, 220, 121, 255 };
static constexpr SDL_Color VibrationColor { 235, 180, 64, 255 };
static constexpr auto VibrationPreview = std::chrono::milliseconds(120);

StopWatchUI::StopWatchUI() : _controls(_layout.device, _layout.toolbar, _layout.toolbarWidth, _layout.selectorRect)
{
	const auto& screen = _layout.device.screen;
	const float centerX = _layout.screenRect.x + screen.width * 0.5f;
	const float centerY = _layout.screenRect.y + screen.height * 0.5f;
	for (int ring = 0; ring < BorderWidth; ++ring)
	{
		const float radius = screen.width * 0.5f + 0.5f + ring;
		for (int i = 0; i <= BorderSegments; ++i)
		{
			const float angle = 2 * std::numbers::pi_v<float> * i / BorderSegments;
			_screenBorders[ring][i] = { centerX + radius * std::cos(angle), centerY + radius * std::sin(angle) };
		}
	}
}

void StopWatchUI::reset()
{
	resetInput();
	_controls.reset();
	_vibrationLevel = 0;
	_vibrationUntil = {};
}

void StopWatchUI::draw(Display& display, const DeviceFeedback& feedback, const Options& options, std::span<const std::filesystem::path> recentFiles, bool recording)
{
	SDL_Renderer* renderer = display.renderer();
	SDL_SetRenderDrawColor(renderer, BackgroundColor.r, BackgroundColor.g, BackgroundColor.b, BackgroundColor.a);
	Display::checkSdl(SDL_RenderClear(renderer) == 0, "clear renderer");
	
	display.drawScreen(_layout.screenRect);
	SDL_SetRenderDrawColor(renderer, ScreenBorderColor.r, ScreenBorderColor.g, ScreenBorderColor.b, ScreenBorderColor.a);
	for (const auto& ring : _screenBorders) Display::checkSdl(SDL_RenderDrawLinesF(renderer, ring.data(), static_cast<int>(ring.size())) == 0, "render screen border");
	
	if (options.flashPath.empty()) drawIdle(display);
	drawControls(display, feedback);
	_controls.drawStatistics(display, feedback, { StopWatchLayout::ScreenMargin, _layout.statisticsTop }, _layout.toolbarWidth, !options.flashPath.empty());
	_controls.draw(display, options, recentFiles, recording);
}

void StopWatchUI::resetInput()
{
	_controls.closeMenus();
	_input.reset();
}

UiAction StopWatchUI::handleEvent(const SDL_Event& event, const Options& options, std::span<const std::filesystem::path> recentFiles, bool running)
{
	const bool menuWasOpen = _controls.menuOpen();
	if (const auto action = _controls.handleEvent(event, options, recentFiles))
	{
		if (menuWasOpen || _controls.menuOpen()) _input.reset();
		return *action;
	}
	if (running) _input.handle(event, _layout.device, _layout.screenRect, _layout.buttonRects);
	return {};
}

void StopWatchUI::setNotice(std::string_view message, bool error)
{
	if (error) resetInput();
	_controls.setNotice(message, error);
}

void StopWatchUI::drawIdle(Display& display)
{
	const auto line = [&] (std::string_view text, int y, int scale, SDL_Color color) {
		const int width = static_cast<int>(text.size()) * Display::GlyphAdvance * scale - scale;
		display.drawText(text, (_layout.width - width) / 2, y, scale, color);
	};
	const int centerY = _layout.screenRect.y + _layout.screenRect.h / 2;
	line("DROP A .BIN FILE", centerY - 24, 3, LabelColor);
	line("APP BIN OR FULL FLASH IMAGE", centerY + 18, 2, MutedColor);
}

void StopWatchUI::drawControls(Display& display, const DeviceFeedback& feedback)
{
	SDL_Renderer* renderer = display.renderer();
	const auto& device = _layout.device;
	for (std::size_t i = 0; i < _layout.buttonRects.size(); ++i)
	{
		const SDL_Rect& rect = _layout.buttonRects[i];
		const SDL_Color color = (feedback.buttons & (1u << i)) ? PressedColor : TrackColor;
		SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
		Display::checkSdl(SDL_RenderFillRect(renderer, &rect) == 0, "render button");
		const auto label = device.buttons[i];
		const int labelWidth = static_cast<int>(label.size()) * Display::GlyphAdvance * 3 - 3;
		display.drawText(label, rect.x + (rect.w - labelWidth) / 2, rect.y + (rect.h - Display::GlyphHeight * 3) / 2, 3, LabelColor);
	}
	
	if (device.powerLed)
	{
		display.drawText("LED", _layout.ledLabel.x, _layout.ledLabel.y, 2, LabelColor);
		const SDL_Color ledColor = feedback.powerLed ? LedColor : TrackColor;
		SDL_SetRenderDrawColor(renderer, ledColor.r, ledColor.g, ledColor.b, ledColor.a);
		const auto& center = _layout.ledCenter;
		constexpr int LedRadius = 7;
		for (int y = -LedRadius; y <= LedRadius; ++y)
		{
			const int halfWidth = static_cast<int>(std::sqrt(LedRadius * LedRadius - y * y));
			Display::checkSdl(SDL_RenderDrawLine(renderer, center.x - halfWidth, center.y + y, center.x + halfWidth, center.y + y) == 0, "render status LED");
		}
	}
	
	if (device.vibration)
	{
		// Retain the last nonzero level briefly so short firmware pulses remain visible.
		const auto now = std::chrono::steady_clock::now();
		if (feedback.vibration)
		{
			_vibrationLevel = feedback.vibration;
			_vibrationUntil = now + VibrationPreview;
		}
		else if (now >= _vibrationUntil) _vibrationLevel = 0;
		display.drawText("VIB", _layout.vibrationLabel.x, _layout.vibrationLabel.y, 2, LabelColor);
		SDL_Rect vibration = _layout.vibrationRect;
		SDL_SetRenderDrawColor(renderer, TrackColor.r, TrackColor.g, TrackColor.b, TrackColor.a);
		Display::checkSdl(SDL_RenderFillRect(renderer, &vibration) == 0, "render vibration track");
		vibration.w = vibration.w * _vibrationLevel / 65535;
		SDL_SetRenderDrawColor(renderer, VibrationColor.r, VibrationColor.g, VibrationColor.b, VibrationColor.a);
		Display::checkSdl(SDL_RenderFillRect(renderer, &vibration) == 0, "render vibration strength");
	}
	
	display.drawText("1/2:A/B `:POWER F5:REBOOT ESC:QUIT", StopWatchLayout::ScreenMargin, _layout.helpTop, 2, MutedColor);
	if (device.motion) display.drawText("ARROWS:TILT Q/E:TURN SPC:SHAKE 0:RESET", StopWatchLayout::ScreenMargin, _layout.helpTop + 20, 2, MutedColor);
}

} // namespace m5emulator
