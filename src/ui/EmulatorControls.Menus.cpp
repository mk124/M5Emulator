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
#include <utility>

#include "Menu.hpp"

namespace m5emulator {

static constexpr SDL_Color MenuBackgroundColor { 24, 28, 34, 255 };
static constexpr SDL_Color MenuBorderColor { 65, 147, 153, 255 };
static constexpr SDL_Color MenuSelectionColor { 25, 116, 125, 255 };
static constexpr SDL_Color MenuTextColor { 235, 241, 247, 255 };
static constexpr SDL_Color MenuMutedColor { 133, 147, 165, 255 };

static constexpr SDL_Color SelectorTextColor { 148, 158, 172, 255 };
static constexpr SDL_Color SelectorColor { 20, 24, 30, 255 };
static constexpr SDL_Color SelectorHoverColor { 30, 38, 48, 255 };

static constexpr SDL_Color EjectColor { 103, 45, 51, 255 };

void EmulatorControls::closeMenus()
{
	_recentSelection.reset();
	_deviceSelection.reset();
}

std::optional<UiAction> EmulatorControls::handleEvent(const SDL_Event& event, const Options& options, std::span<const std::filesystem::path> recentFiles)
{
	using Type = UiAction::Type;
	const bool key = event.type == SDL_KEYDOWN && !event.key.repeat;
	const bool click = event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT;
	const SDL_Point point { click ? event.button.x : 0, click ? event.button.y : 0 };
	if ((key && event.key.keysym.sym == SDLK_F4) || (click && SDL_PointInRect(&point, &_selectorRect)))
	{
		toggleDeviceMenu();
		return UiAction {};
	}
	
	if (key && event.key.keysym.sym >= SDLK_F5 && event.key.keysym.sym <= SDLK_F9 && !(event.key.keysym.sym == SDLK_F6 && _recentSelection))
	{
		_deviceSelection.reset();
		_recentSelection.reset();
	}
	
	if (_deviceSelection)
	{
		const MenuAction action = handleMenuInput(event, _deviceSelection, deviceMenuRect(), deviceRowRect(0), Devices.size());
		if (action == MenuAction::Activate)
		{
			const std::size_t index = *_deviceSelection;
			_deviceSelection.reset();
			return UiAction { Type::SelectDevice, index };
		}
		if (action != MenuAction::PassThrough) return UiAction {};
	}
	if (_recentSelection)
	{
		const std::size_t count = recentFiles.size() + 1;
		const MenuAction action = handleMenuInput(event, _recentSelection, recentMenuRect(count), recentRowRect(0, count), count, SDLK_F6);
		if (action == MenuAction::Activate)
		{
			const std::size_t index = *_recentSelection;
			if (index < recentFiles.size())
			{
				std::error_code error;
				if (!std::filesystem::is_regular_file(recentFiles[index], error)) return UiAction {};
			}
			_recentSelection.reset();
			return UiAction { index == recentFiles.size() ? Type::Eject : Type::OpenRecent, index };
		}
		if (action != MenuAction::PassThrough) return UiAction {};
	}
	
	if (key)
	{
		switch (event.key.keysym.sym)
		{
			case SDLK_ESCAPE: return UiAction { Type::Quit };
			case SDLK_F5:     return UiAction { Type::Restart };
			case SDLK_F6:     {
				toggleRecentMenu(!recentFiles.empty() || !options.flashPath.empty());
				return UiAction {};
			}
			case SDLK_F7: return UiAction { Type::Eject };
			case SDLK_F8: return UiAction { Type::Capture };
			case SDLK_F9: return UiAction { Type::Record };
			default:      break;
		}
	}
	if (click)
	{
		if (SDL_PointInRect(&point, &_recentRect))
		{
			toggleRecentMenu(!recentFiles.empty() || !options.flashPath.empty());
			return UiAction {};
		}
		if (SDL_PointInRect(&point, &_captureRect)) return UiAction { Type::Capture };
		if (SDL_PointInRect(&point, &_recordRect)) return UiAction { Type::Record };
		for (std::size_t i = 0; i < _settingRects.size(); ++i)
		{
			if (SDL_PointInRect(&point, &_settingRects[i])) return UiAction { Type::ChangeSetting, i };
		}
	}
	return std::nullopt;
}

void EmulatorControls::toggleDeviceMenu()
{
	if (_deviceSelection)
	{
		_deviceSelection.reset();
		return;
	}
	closeMenus();
	for (std::size_t i = 0; i < Devices.size(); ++i)
	{
		if (Devices[i].id == _device.id) _deviceSelection = i;
	}
	setNotice({});
}

void EmulatorControls::toggleRecentMenu(bool available)
{
	_deviceSelection.reset();
	if (_recentSelection)
	{
		_recentSelection.reset();
		return;
	}
	if (!available) return;
	closeMenus();
	_recentSelection = 0;
	_recentDirty = true;
	setNotice({});
}

SDL_Rect EmulatorControls::recentMenuRect(std::size_t count) const
{
	const int menuHeight = RecentHeaderHeight + static_cast<int>(count) * MenuRowHeight + RecentDetailsHeight;
	return { _toolbar.x, _toolbar.y - MenuGap - menuHeight, _width, menuHeight };
}

SDL_Rect EmulatorControls::recentRowRect(std::size_t index, std::size_t count) const
{
	const SDL_Rect menu = recentMenuRect(count);
	return { menu.x + MenuPadding, menu.y + RecentHeaderHeight + static_cast<int>(index) * MenuRowHeight, menu.w - MenuPadding * 2, MenuRowHeight };
}

SDL_Rect EmulatorControls::deviceMenuRect() const
{
	int menuWidth = 0;
	for (const auto& item : Devices) menuWidth = std::max(menuWidth, tagWidth(item.name) + MenuPadding * 2);
	return { _selectorRect.x + _selectorRect.w - menuWidth, _selectorRect.y + _selectorRect.h + MenuGap, menuWidth, static_cast<int>(Devices.size()) * MenuRowHeight + MenuPadding * 2 };
}

SDL_Rect EmulatorControls::deviceRowRect(std::size_t index) const
{
	const SDL_Rect menu = deviceMenuRect();
	return { menu.x + MenuPadding, menu.y + MenuPadding + static_cast<int>(index) * MenuRowHeight, menu.w - MenuPadding * 2, MenuRowHeight };
}

void EmulatorControls::drawDeviceSelector(Display& display, bool open)
{
	const auto& device = _device;
	const SDL_Rect rect = _selectorRect;
	SDL_Point mouse;
	SDL_GetMouseState(&mouse.x, &mouse.y);
	const bool highlighted = open || SDL_PointInRect(&mouse, &rect);
	const SDL_Color background = highlighted ? SelectorHoverColor : SelectorColor;
	display.drawTag(device.label, rect, TagPadding, TagScale, background, SelectorTextColor);
	
	const int arrowX = rect.x + rect.w - TagPadding - GlyphWidth * TagScale;
	const int arrowY = rect.y + (rect.h - DeviceArrowHeight * TagScale) / 2;
	SDL_SetRenderDrawColor(display.renderer(), SelectorTextColor.r, SelectorTextColor.g, SelectorTextColor.b, SelectorTextColor.a);
	for (int row = 0; row < DeviceArrowHeight; ++row)
	{
		const int inset = open ? DeviceArrowHeight - 1 - row : row;
		const SDL_Rect dots { arrowX + inset * TagScale, arrowY + row * TagScale, (GlyphWidth - inset * 2) * TagScale, TagScale };
		Display::checkSdl(SDL_RenderFillRect(display.renderer(), &dots) == 0, "render device arrow");
	}
}

void EmulatorControls::drawDeviceMenu(Display& display, std::size_t selection)
{
	const SDL_Rect bounds = deviceMenuRect();
	SDL_Renderer* renderer = display.renderer();
	SDL_SetRenderDrawColor(renderer, MenuBackgroundColor.r, MenuBackgroundColor.g, MenuBackgroundColor.b, MenuBackgroundColor.a);
	Display::checkSdl(SDL_RenderFillRect(renderer, &bounds) == 0, "render device menu");
	SDL_SetRenderDrawColor(renderer, MenuBorderColor.r, MenuBorderColor.g, MenuBorderColor.b, MenuBorderColor.a);
	Display::checkSdl(SDL_RenderDrawRect(renderer, &bounds) == 0, "render device border");
	
	for (std::size_t i = 0; i < Devices.size(); ++i)
	{
		const SDL_Rect row = deviceRowRect(i);
		if (selection == i)
		{
			SDL_SetRenderDrawColor(renderer, MenuSelectionColor.r, MenuSelectionColor.g, MenuSelectionColor.b, MenuSelectionColor.a);
			Display::checkSdl(SDL_RenderFillRect(renderer, &row) == 0, "render device selection");
		}
		display.drawText(Devices[i].name, row.x + TagPadding, row.y + (row.h - GlyphHeight * TagScale) / 2, TagScale, MenuTextColor);
	}
}

void EmulatorControls::prepareRecentFiles(Display& display, std::span<const std::filesystem::path> files)
{
	static_assert(PixelFont::Height == FileTextHeight);
	if (!_fileFont)
	{
		const std::unique_ptr<char, decltype(&SDL_free)> directory(SDL_GetBasePath(), SDL_free);
		Display::checkSdl(directory != nullptr, "locate font resources");
		_fileFont.emplace(std::filesystem::path(directory.get()) / "fonts/unifont-15.1.05.hex");
	}
	
	std::vector<Texture> names, paths;
	std::vector<bool> available;
	const int width = recentMenuRect(files.size()).w - MenuPadding * 2;
	const auto texture = [&] (std::string_view text, int lines) {
		const auto surface = _fileFont->render(text, width, lines);
		Texture result(SDL_CreateTextureFromSurface(display.renderer(), surface.get()), SDL_DestroyTexture);
		Display::checkSdl(result != nullptr, "create file label texture");
		Display::checkSdl(SDL_SetTextureScaleMode(result.get(), SDL_ScaleModeNearest) == 0, "set pixel text scaling");
		return result;
	};
	
	for (const auto& file : files)
	{
		names.push_back(texture(file.filename().string(), 1));
		paths.push_back(texture(file.string(), RecentPathLines));
		std::error_code error;
		available.push_back(std::filesystem::is_regular_file(file, error));
	}
	
	_recentNames = std::move(names);
	_recentPaths = std::move(paths);
	_recentAvailable = std::move(available);
}

void EmulatorControls::drawRecentFiles(Display& display, std::size_t selection, const Options& options)
{
	const std::size_t count = _recentNames.size() + 1;
	const SDL_Rect bounds = recentMenuRect(count);
	SDL_Renderer* renderer = display.renderer();
	SDL_SetRenderDrawColor(renderer, MenuBackgroundColor.r, MenuBackgroundColor.g, MenuBackgroundColor.b, MenuBackgroundColor.a);
	Display::checkSdl(SDL_RenderFillRect(renderer, &bounds) == 0, "render recent menu");
	SDL_SetRenderDrawColor(renderer, MenuBorderColor.r, MenuBorderColor.g, MenuBorderColor.b, MenuBorderColor.a);
	Display::checkSdl(SDL_RenderDrawRect(renderer, &bounds) == 0, "render recent border");
	
	display.drawText(_device.name, bounds.x + MenuPadding, bounds.y + MenuPadding, TagScale, MenuTextColor);
	constexpr std::string_view CloseHint = "ESC:CLOSE";
	const int hintWidth = static_cast<int>(CloseHint.size()) * GlyphAdvance * TagScale - TagScale;
	display.drawText(CloseHint, bounds.x + bounds.w - MenuPadding - hintWidth, bounds.y + MenuPadding, TagScale, MenuMutedColor);
	
	for (std::size_t i = 0; i < _recentNames.size(); ++i)
	{
		SDL_Rect row = recentRowRect(i, count);
		if (i == selection)
		{
			SDL_SetRenderDrawColor(renderer, MenuSelectionColor.r, MenuSelectionColor.g, MenuSelectionColor.b, MenuSelectionColor.a);
			Display::checkSdl(SDL_RenderFillRect(renderer, &row) == 0, "render recent selection");
		}
		
		row.y += (row.h - PixelFont::Height) / 2;
		row.h = PixelFont::Height;
		const SDL_Color color = _recentAvailable[i] ? MenuTextColor : MenuMutedColor;
		SDL_SetTextureColorMod(_recentNames[i].get(), color.r, color.g, color.b);
		Display::checkSdl(SDL_RenderCopy(renderer, _recentNames[i].get(), nullptr, &row) == 0, "render recent filename");
	}
	
	const SDL_Rect eject = recentRowRect(_recentNames.size(), count);
	if (selection == _recentNames.size())
	{
		SDL_SetRenderDrawColor(renderer, EjectColor.r, EjectColor.g, EjectColor.b, EjectColor.a);
		Display::checkSdl(SDL_RenderFillRect(renderer, &eject) == 0, "render eject selection");
	}
	display.drawText("EJT  EJECT CURRENT BIN   F7", eject.x + TagPadding, eject.y + (eject.h - GlyphHeight * 2) / 2, 2, options.flashPath.empty() ? MenuMutedColor : MenuTextColor);
	
	const SDL_Rect details { bounds.x + MenuPadding, bounds.y + bounds.h - RecentDetailsHeight + MenuPadding, bounds.w - MenuPadding * 2, PixelFont::Height * RecentPathLines };
	SDL_SetRenderDrawColor(renderer, MenuBorderColor.r, MenuBorderColor.g, MenuBorderColor.b, MenuBorderColor.a);
	Display::checkSdl(SDL_RenderDrawLine(renderer, details.x, details.y - MenuPadding / 2, details.x + details.w, details.y - MenuPadding / 2) == 0, "render recent divider");
	if (selection == _recentNames.size())
	{
		display.drawText(options.flashPath.empty() ? "NO FIRMWARE LOADED" : "UNLOAD CURRENT FIRMWARE", details.x, details.y, 2, MenuTextColor);
		if (!options.flashPath.empty()) display.drawText(options.persistFlash ? "SAVED FLASH DATA IS KEPT" : "TEMPORARY FLASH IS REMOVED", details.x, details.y + 20, 2, MenuMutedColor);
		return;
	}
	
	SDL_SetTextureColorMod(_recentPaths[selection].get(), MenuMutedColor.r, MenuMutedColor.g, MenuMutedColor.b);
	Display::checkSdl(SDL_RenderCopy(renderer, _recentPaths[selection].get(), nullptr, &details) == 0, "render recent path");
}

} // namespace m5emulator
