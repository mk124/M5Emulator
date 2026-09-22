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
#include <string_view>
#include <vector>

namespace m5emulator {

class RecentFiles
{
public:
	explicit RecentFiles(std::string_view deviceId, const std::filesystem::path& storageDirectory = {});
	void remember(const std::filesystem::path& path);
	
public:
	const std::vector<std::filesystem::path>& entries() const { return _entries; }
	
private:
	std::filesystem::path _storagePath;
	std::vector<std::filesystem::path> _entries;
};

} // namespace m5emulator
