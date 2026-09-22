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
#include <filesystem>
#include <thread>

namespace m5emulator {

// Tee a process output descriptor, including inherited child output.
class OutputLog
{
public:
	OutputLog(int descriptor, const std::filesystem::path& path);
	~OutputLog();
	
	OutputLog(const OutputLog&) = delete;
	OutputLog& operator=(const OutputLog&) = delete;
	
private:
	void forward() noexcept;
	void close() noexcept;
	
private:
	int _descriptor;
	int _original = -1;
	int _file = -1;
	std::array<int, 2> _pipe { -1, -1 };
	std::thread _reader;
};

} // namespace m5emulator
