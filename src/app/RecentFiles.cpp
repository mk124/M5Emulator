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

#include "RecentFiles.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

#include <SDL.h>
#include <unistd.h>

namespace m5emulator {

namespace fs = std::filesystem;

static constexpr std::size_t RecentLimit = 8;

RecentFiles::RecentFiles(std::string_view deviceId, const fs::path& storageDirectory)
{
	fs::path base = storageDirectory;
	if (base.empty())
	{
		const std::unique_ptr<char, decltype(&SDL_free)> directory(SDL_GetPrefPath("", "M5Emulator"), SDL_free);
		if (!directory)
		{
			std::cerr << "Cannot locate recent-file storage: " << SDL_GetError() << '\n';
			return;
		}
		base = directory.get();
	}
	
	if (deviceId.empty() || deviceId.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string_view::npos) throw std::runtime_error("Invalid recent-file device ID");
	_storagePath = base / "devices" / deviceId / "recent-files.txt";
	
	std::error_code error;
	const std::uintmax_t size = fs::file_size(_storagePath, error);
	if (error || size > 64 * 1024) return;
	
	std::ifstream input(_storagePath, std::ios::binary);
	std::string entry;
	while (_entries.size() < RecentLimit && input >> std::quoted(entry))
	{
		const fs::path path(entry);
		if (path.is_absolute() && std::ranges::find(_entries, path) == _entries.end()) _entries.push_back(path);
	}
}

void RecentFiles::remember(const fs::path& path)
{
	const fs::path absolute = fs::weakly_canonical(fs::absolute(path));
	std::erase(_entries, absolute);
	_entries.insert(_entries.begin(), absolute);
	if (_entries.size() > RecentLimit) _entries.resize(RecentLimit);
	if (_storagePath.empty()) return;
	
	const fs::path temporary = _storagePath.string() + ".tmp-" + std::to_string(getpid());
	try
	{
		fs::create_directories(_storagePath.parent_path());
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		for (const auto& entry : _entries) output << std::quoted(entry.string()) << '\n';
		output.close();
		if (!output) throw std::runtime_error("Cannot save recent BIN files");
		fs::rename(temporary, _storagePath);
	}
	catch (...)
	{
		std::error_code ignored;
		fs::remove(temporary, ignored);
		throw;
	}
}

} // namespace m5emulator
