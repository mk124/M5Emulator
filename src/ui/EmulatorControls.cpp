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

#include "EmulatorControls.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>

#include "Setting.hpp"

namespace m5emulator {

static constexpr auto StatisticsInterval = std::chrono::seconds(1);
static constexpr SDL_Color TextColor { 215, 222, 232, 255 };
static constexpr SDL_Color MutedColor { 133, 147, 165, 255 };
static constexpr SDL_Color InactiveColor { 38, 43, 50, 255 };
static constexpr SDL_Color RecentColor { 25, 116, 125, 255 };
static constexpr SDL_Color CaptureColor { 78, 101, 125, 255 };
static constexpr SDL_Color RecordColor { 103, 45, 51, 255 };
static constexpr SDL_Color RecordingColor { 211, 44, 52, 255 };
static constexpr SDL_Color RecordingLampColor { 255, 75, 75, 255 };
static constexpr SDL_Color ActiveTextColor { 255, 255, 255, 255 };

static constexpr std::array SettingColors {
	SDL_Color { 25, 124, 83, 255 }, // Speaker green
	SDL_Color { 173, 85, 18, 255 }, // Microphone amber
	SDL_Color { 30, 100, 210, 255 }, // Bluetooth blue
	SDL_Color { 139, 108, 31, 255 }, // Persistence gold
	SDL_Color { 111, 76, 178, 255 }, // Timing purple
	SDL_Color { 158, 71, 102, 255 } // Brightness rose
};

static constexpr SDL_Color ErrorColor { 243, 128, 116, 255 };
static constexpr SDL_Color SuccessColor { 135, 213, 164, 255 };
static constexpr SDL_Color BackgroundColor { 24, 28, 34, 255 };
static constexpr SDL_Color BorderColor { 68, 82, 103, 255 };
static constexpr auto NoticeDuration = std::chrono::seconds(3);

SDL_Point EmulatorControls::selectorSize(const Device& device)
{
	return { tagWidth(device.label) + DeviceArrowGap + GlyphWidth * TagScale, Height };
}

EmulatorControls::EmulatorControls(const Device& device, SDL_Point toolbar, int width, const SDL_Rect& selector)
	: _device(device), _toolbar(toolbar), _width(width), _selectorRect(selector)
{
	int x = toolbar.x;
	for (std::size_t i = 0; i < _settingRects.size(); ++i)
	{
		_settingRects[i] = { x, toolbar.y, tagWidth(SettingLabels[i]), TagHeight };
		x += _settingRects[i].w + SettingGap;
	}
	_recentRect = { x + SettingGap, toolbar.y, tagWidth(RecentLabel), TagHeight };
	_captureRect = { _recentRect.x + _recentRect.w + SettingGap, toolbar.y, tagWidth(CaptureLabel), TagHeight };
	_recordRect = { _captureRect.x + _captureRect.w + SettingGap, toolbar.y, tagWidth(RecordLabel), TagHeight };
	_noticeTop = toolbar.y - 48;
	_noticeRect = { toolbar.x - NoticePadding, _noticeTop - NoticePadding, width + NoticePadding * 2, NoticeLineStep + GlyphHeight * TagScale + NoticePadding * 2 };
	_recordingLampRect = { _recordRect.x + (_recordRect.w - RecordingLampSize) / 2, _recordRect.y - RecordingLampGap - RecordingLampSize, RecordingLampSize, RecordingLampSize };
}

void EmulatorControls::reset()
{
	closeMenus();
	_statisticsTime = std::chrono::steady_clock::now();
	_statisticsGuestTimeNs = _statisticsRefreshes = 0;
	_statisticsPresentations = _presentations;
	_fpsText = "FPS --";
	_presentationFpsText = "SDL --";
	_speedText = "SIM --x";
	_noticeText.clear();
}

void EmulatorControls::draw(Display& display, const Options& options, std::span<const std::filesystem::path> recentFiles, bool recording)
{
	if (_recentSelection && _recentDirty)
	{
		try
		{
			prepareRecentFiles(display, recentFiles);
			_recentDirty = false;
		}
		catch (const std::exception& error)
		{
			std::cerr << "Error: " << error.what() << '\n';
			setNotice(error.what(), true);
		}
	}
	const bool hasRecent = !recentFiles.empty();
	drawSettings(display, options, hasRecent, recording);
	drawNotice(display, options, hasRecent, recording);
	drawDeviceSelector(display, _deviceSelection.has_value());
	if (_recentSelection) drawRecentFiles(display, *_recentSelection, options);
	if (_deviceSelection) drawDeviceMenu(display, *_deviceSelection);
	++_presentations;
}

void EmulatorControls::drawStatistics(Display& display, const DeviceFeedback& feedback, SDL_Point position, int width, bool running)
{
	if (running) updateStatistics(feedback);
	display.drawText(_fpsText, position.x, position.y, 2, TextColor);
	display.drawText(_presentationFpsText, position.x + width / 3, position.y, 2, TextColor);
	display.drawText(_speedText, position.x + width / 3 * 2, position.y, 2, TextColor);
}

void EmulatorControls::setNotice(std::string_view message, bool error)
{
	if (error) closeMenus();
	_noticeText = message;
	_noticeError = error;
	_noticeUntil = std::chrono::steady_clock::now() + NoticeDuration;
	std::ranges::transform(_noticeText, _noticeText.begin(), [] (unsigned char character) {
		return character >= 32 && character < 127 ? std::toupper(character) : ' ';
	});
}

void EmulatorControls::updateStatistics(const DeviceFeedback& feedback)
{
	const auto now = std::chrono::steady_clock::now();
	const auto elapsed = now - _statisticsTime;
	const bool restarted = feedback.guestTimeNs < _statisticsGuestTimeNs || feedback.activeRefreshes < _statisticsRefreshes;
	if (elapsed < StatisticsInterval && !restarted) return;
	
	if (restarted)
	{
		_fpsText = "FPS --";
		_presentationFpsText = "SDL --";
		_speedText = "SIM --x";
	}
	else
	{
		const double seconds = std::chrono::duration<double>(elapsed).count();
		const double fps = (feedback.activeRefreshes - _statisticsRefreshes) / seconds;
		const double presentationFps = (_presentations - _statisticsPresentations) / seconds;
		const double speed = (feedback.guestTimeNs - _statisticsGuestTimeNs) / (seconds * 1e9);
		
		std::ostringstream text;
		text.imbue(std::locale::classic());
		text << std::fixed << std::setprecision(1) << "FPS " << fps;
		_fpsText = text.str();
		text.str({});
		text << "SDL " << presentationFps;
		_presentationFpsText = text.str();
		text.str({});
		text << std::setprecision(2) << "SIM " << speed << 'x';
		_speedText = text.str();
	}
	
	_statisticsTime = now;
	_statisticsGuestTimeNs = feedback.guestTimeNs;
	_statisticsRefreshes = feedback.activeRefreshes;
	_statisticsPresentations = _presentations;
}

void EmulatorControls::drawSettings(Display& display, const Options& options, bool hasRecent, bool recording)
{
	static_assert(SettingLabels.size() == SettingColors.size());
	const bool bluetoothAvailable = options.controllerHle;
	const std::array enabled { options.audio, options.microphone, options.bluetooth && bluetoothAvailable, options.persistFlash, options.icount, options.simulateBrightness };
	static_assert(enabled.size() == SettingLabels.size());
	SDL_Renderer* renderer = display.renderer();
	const auto tag = [&] (std::string_view label, const SDL_Rect& rect, SDL_Color color, bool active) {
		if (!active) color = InactiveColor;
		display.drawTag(label, rect, TagPadding, TagScale, color, active ? ActiveTextColor : TextColor);
	};
	
	for (std::size_t i = 0; i < SettingLabels.size(); ++i) tag(SettingLabels[i], _settingRects[i], SettingColors[i], enabled[i]);
	tag(RecentLabel, _recentRect, RecentColor, hasRecent || !options.flashPath.empty());
	tag(CaptureLabel, _captureRect, CaptureColor, !options.flashPath.empty());
	tag(RecordLabel, _recordRect, recording ? RecordingColor : RecordColor, !options.flashPath.empty());
	
	if (recording)
	{
		// A small lamp remains visible even while another hint is displayed.
		const SDL_Rect& lamp = _recordingLampRect;
		SDL_SetRenderDrawColor(renderer, RecordingLampColor.r, RecordingLampColor.g, RecordingLampColor.b, RecordingLampColor.a);
		Display::checkSdl(SDL_RenderFillRect(renderer, &lamp) == 0, "render recording lamp");
	}
}

void EmulatorControls::drawNotice(Display& display, const Options& options, bool hasRecent, bool recording)
{
	SDL_Renderer* renderer = display.renderer();
	const bool bluetoothAvailable = options.controllerHle;
	
	const auto drawNoticeBackground = [&] {
		const SDL_Rect& rect = _noticeRect;
		SDL_SetRenderDrawColor(renderer, BackgroundColor.r, BackgroundColor.g, BackgroundColor.b, BackgroundColor.a);
		Display::checkSdl(SDL_RenderFillRect(renderer, &rect) == 0, "render notice background");
		SDL_SetRenderDrawColor(renderer, BorderColor.r, BorderColor.g, BorderColor.b, BorderColor.a);
		Display::checkSdl(SDL_RenderDrawRect(renderer, &rect) == 0, "render notice border");
	};
	if (!_noticeText.empty() && (_noticeError || std::chrono::steady_clock::now() < _noticeUntil))
	{
		drawNoticeBackground();
		const std::size_t lineLength = _width / (GlyphAdvance * 2);
		std::string_view remaining = _noticeText;
		for (int row = 0; row < 2 && !remaining.empty(); ++row)
		{
			std::size_t count = std::min(remaining.size(), lineLength);
			if (count < remaining.size())
			{
				const std::size_t space = remaining.rfind(' ', count);
				if (space != std::string_view::npos && space) count = space;
			}
			display.drawText(remaining.substr(0, count), _toolbar.x, _noticeTop + row * NoticeLineStep, 2, _noticeError ? ErrorColor : SuccessColor);
			remaining.remove_prefix(count);
			while (remaining.starts_with(' ')) remaining.remove_prefix(1);
		}
		return;
	}
	
	SDL_Point mouse;
	SDL_GetMouseState(&mouse.x, &mouse.y);
	std::string_view hint, effect;
	if (SDL_PointInRect(&mouse, &_recentRect))
	{
		hint = "F6: BIN FILES / EJECT";
		effect = hasRecent ? (options.persistFlash ? "RESUMES SAVED FLASH DATA" : "LOADS A FRESH TEMPORARY FLASH") : "NO RECENT FILES YET";
	}
	else if (SDL_PointInRect(&mouse, &_captureRect))
	{
		hint = "F8: CAPTURE SCREEN";
		effect = options.flashPath.empty() ? "NO FIRMWARE LOADED" : "SAVES A BMP IMAGE";
	}
	else if (SDL_PointInRect(&mouse, &_recordRect))
	{
		hint = recording ? "F9: STOP RECORDING" : "F9: RECORD SCREEN AND SOUND";
		effect = options.flashPath.empty() ? "NO FIRMWARE LOADED" : "SAVES MP4 / H.265 + AAC";
	}
	else
	{
		for (std::size_t i = 0; i < _settingRects.size(); ++i)
		{
			if (!SDL_PointInRect(&mouse, &_settingRects[i])) continue;
			effect = "RESTARTS DEVICE / FLASH IS KEPT";
			switch (static_cast<Setting>(i))
			{
				case Setting::Bluetooth: {
					hint = "BLE: HOST BLUETOOTH";
					effect = bluetoothAvailable ? "FIRST ENABLE MAY RESTART DEVICE" : "FIRST ENABLE RESTARTS DEVICE";
					break;
				}
				case Setting::Sound: {
					hint = "SND: SPEAKER OUTPUT";
					break;
				}
				case Setting::Microphone: {
					hint = "MIC: HOST MICROPHONE";
					break;
				}
				case Setting::Persistence: {
					hint = "PST: PERSISTENT FLASH";
					if (options.flashPath.empty()) effect = "KEEPS FLASH DATA BETWEEN RUNS";
					break;
				}
				case Setting::Icount: {
					hint = "ICN: INSTRUCTION-COUNT TIMING";
					break;
				}
				case Setting::Brightness: {
					hint = "BRT: SIMULATE PANEL BRIGHTNESS";
					effect = "APPLIES LIVE / DEFAULT OFF";
					break;
				}
			}
			break;
		}
	}
	if (hint.empty()) return;
	drawNoticeBackground();
	display.drawText(hint, _toolbar.x, _noticeTop, 2, TextColor);
	display.drawText(effect, _toolbar.x, _noticeTop + NoticeLineStep, 2, MutedColor);
}

} // namespace m5emulator
