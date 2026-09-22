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
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Display.hpp"
#include "PixelFont.hpp"
#include "UiAction.hpp"

namespace m5emulator {

class EmulatorControls
{
	using Texture = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;
	
	static constexpr int GlyphWidth = Display::GlyphWidth, GlyphHeight = Display::GlyphHeight, GlyphAdvance = Display::GlyphAdvance;
	static constexpr int TagScale = 2, TagPadding = TagScale * 2;
	static constexpr int TagHeight = GlyphHeight * TagScale + TagPadding * 2;
	static constexpr int SettingGap = TagScale * 4;
	
	static constexpr std::array<std::string_view, 6> SettingLabels { "SND", "MIC", "BLE", "PST", "ICN", "BRT" };
	static constexpr std::string_view RecentLabel = "BIN", CaptureLabel = "CAP", RecordLabel = "REC";
	static constexpr int NoticePadding = 8, NoticeLineStep = 20;
	static constexpr int RecordingLampSize = 4, RecordingLampGap = 3;
	static constexpr int MenuPadding = 8, MenuRowHeight = 24, MenuGap = 8;
	static constexpr int DeviceArrowGap = TagPadding * 2;
	static constexpr int DeviceArrowHeight = (GlyphWidth + 1) / 2;
	static constexpr int RecentHeaderHeight = GlyphHeight * TagScale + MenuPadding * 2;
	static constexpr int FileTextHeight = 16, RecentPathLines = 3;
	static constexpr int RecentDetailsHeight = FileTextHeight * RecentPathLines + MenuPadding * 2;
	
public:
	static constexpr int Height = TagHeight;
	static SDL_Point selectorSize(const Device& device);
	
private:
	static constexpr int tagWidth(std::string_view label)
	{
		return static_cast<int>(label.size()) * GlyphAdvance * TagScale - TagScale + TagPadding * 2;
	}
	
public:
	EmulatorControls(const Device& device, SDL_Point toolbar, int width, const SDL_Rect& selector);
	void reset();
	void draw(Display& display, const Options& options, std::span<const std::filesystem::path> recentFiles, bool recording);
	void drawStatistics(Display& display, const DeviceFeedback& feedback, SDL_Point position, int width, bool running);
	void setNotice(std::string_view message, bool error = false);
	
	void closeMenus();
	// nullopt leaves the event to the device UI; an action (including None) consumes it.
	std::optional<UiAction> handleEvent(const SDL_Event& event, const Options& options, std::span<const std::filesystem::path> recentFiles);
	
private:
	void updateStatistics(const DeviceFeedback& feedback);
	void drawSettings(Display& display, const Options& options, bool hasRecent, bool recording);
	void drawNotice(Display& display, const Options& options, bool hasRecent, bool recording);
	
	void toggleDeviceMenu();
	void toggleRecentMenu(bool available);
	SDL_Rect recentMenuRect(std::size_t count) const;
	SDL_Rect recentRowRect(std::size_t index, std::size_t count) const;
	SDL_Rect deviceMenuRect() const;
	SDL_Rect deviceRowRect(std::size_t index) const;
	void drawDeviceSelector(Display& display, bool open);
	void drawDeviceMenu(Display& display, std::size_t selection);
	void prepareRecentFiles(Display& display, std::span<const std::filesystem::path> files);
	void drawRecentFiles(Display& display, std::size_t selection, const Options& options);
	
public:
	bool menuOpen() const { return _recentSelection.has_value() || _deviceSelection.has_value(); }
	
private:
	const Device& _device;
	SDL_Point _toolbar;
	int _width;
	SDL_Rect _selectorRect;
	std::array<SDL_Rect, SettingLabels.size()> _settingRects {};
	SDL_Rect _recentRect {}, _captureRect {}, _recordRect {}, _noticeRect {}, _recordingLampRect {};
	int _noticeTop = 0;
	
	std::chrono::steady_clock::time_point _statisticsTime = std::chrono::steady_clock::now();
	uint64_t _statisticsGuestTimeNs = 0, _statisticsRefreshes = 0;
	uint64_t _presentations = 0, _statisticsPresentations = 0;
	std::string _fpsText = "FPS --", _presentationFpsText = "SDL --", _speedText = "SIM --x";
	
	std::string _noticeText;
	bool _noticeError = false;
	std::chrono::steady_clock::time_point _noticeUntil {};
	
	std::optional<std::size_t> _recentSelection, _deviceSelection;
	bool _recentDirty = false;
	std::optional<PixelFont> _fileFont;
	std::vector<Texture> _recentNames, _recentPaths;
	std::vector<bool> _recentAvailable;
};

} // namespace m5emulator
