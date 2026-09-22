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

#include "Test.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "app/RecentFiles.hpp"

namespace m5emulator {

namespace fs = std::filesystem;

static void reopeningFilesKeepsDistinctRecentEntriesAcrossLaunches(const fs::path& directory)
{
	const fs::path storage = directory / "history";
	const std::array names { "计时器 \"一\".bin", "two.bin", "three.bin", "four.bin", "five.bin", "six.bin", "seven.bin", "eight.bin", "nine.bin" };
	for (const char* name : names) std::ofstream(directory / name).put('x');
	
	RecentFiles history("stopwatch", storage);
	for (std::size_t i = 0; i < 8; ++i) history.remember(directory / names[i]);
	fs::create_symlink(directory / names[0], directory / "alias.bin");
	history.remember(directory / "alias.bin");
	history.remember(directory / names[8]);
	
	const RecentFiles reloaded("stopwatch", storage);
	const auto& entries = reloaded.entries();
	CHECK(entries.size() == 8);
	// Reopening the first image protects it from eviction; the second is now oldest.
	const std::array expected { names[8], names[0], names[7], names[6], names[5], names[4], names[3], names[2] };
	for (std::size_t i = 0; i < expected.size(); ++i) CHECK(entries[i] == fs::canonical(directory / expected[i]));
}

static void deviceHistoriesDoNotLeakOrEvictEachOthersFiles(const fs::path& directory)
{
	const fs::path storage = directory / "devices-test";
	const fs::path shared = directory / "shared.bin", watchOnly = directory / "watch.bin";
	std::ofstream(shared).put('x');
	std::ofstream(watchOnly).put('x');
	RecentFiles watch("stopwatch", storage);
	watch.remember(watchOnly);
	watch.remember(shared);
	RecentFiles other("other-device", storage);
	CHECK(other.entries().empty());
	other.remember(shared);
	for (int i = 0; i < 9; ++i)
	{
		const fs::path path = directory / ("other-" + std::to_string(i) + ".bin");
		std::ofstream(path).put('x');
		other.remember(path);
	}
	const RecentFiles reloadedWatch("stopwatch", storage), reloadedOther("other-device", storage);
	CHECK(reloadedWatch.entries().size() == 2);
	CHECK(reloadedWatch.entries()[0] == fs::canonical(shared));
	CHECK(reloadedWatch.entries()[1] == fs::canonical(watchOnly));
	CHECK(reloadedOther.entries().size() == 8);
	CHECK(reloadedOther.entries().front() == fs::canonical(directory / "other-8.bin"));
}

} // namespace m5emulator

int main()
{
	std::string path = (std::filesystem::temp_directory_path() / "m5emu-recent-XXXXXX").string();
	if (mkdtemp(path.data()) == nullptr) return EXIT_FAILURE;
	int result = EXIT_SUCCESS;
	try
	{
		m5emulator::reopeningFilesKeepsDistinctRecentEntriesAcrossLaunches(path);
		m5emulator::deviceHistoriesDoNotLeakOrEvictEachOthersFiles(path);
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		result = EXIT_FAILURE;
	}
	std::filesystem::remove_all(path);
	return result;
}
