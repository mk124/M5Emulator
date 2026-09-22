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

#include "app/Application.hpp"
#include "app/Options.hpp"
#include "app/OutputLog.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

#include <SDL.h>
#include <unistd.h>

namespace m5emulator {

static volatile std::sig_atomic_t stopSignal = 0;

static void onSignal(int signal) { stopSignal = signal; }

static std::filesystem::path createLogDirectory()
{
	const std::unique_ptr<char, decltype(&SDL_free)> base(SDL_GetPrefPath("", "M5Emulator"), SDL_free);
	if (!base) throw std::runtime_error("Cannot locate log storage: " + std::string(SDL_GetError()));
	const auto logs = std::filesystem::path(base.get()) / "logs";
	std::filesystem::create_directories(logs);
	
	const std::time_t now = std::time(nullptr);
	std::tm localTime {};
	char name[64];
	if (localtime_r(&now, &localTime) == nullptr || !std::strftime(name, sizeof(name), "%Y%m%d-%H%M%S-XXXXXX", &localTime))
	{
		throw std::runtime_error("Cannot format log timestamp");
	}
	std::string path = (logs / name).string();
	if (mkdtemp(path.data()) == nullptr) throw std::system_error(errno, std::generic_category(), "create log directory");
	return path;
}

} // namespace m5emulator

int main(int argc, char** argv)
{
	std::optional<m5emulator::OutputLog> outputLog, errorLog;
	try
	{
		const auto directory = m5emulator::createLogDirectory();
		outputLog.emplace(STDOUT_FILENO, directory / "stdout.log");
		errorLog.emplace(STDERR_FILENO, directory / "stderr.log");
		std::cout << "Logs: " << directory << '\n' << std::flush;
	}
	catch (const std::exception& error)
	{
		errorLog.reset();
		outputLog.reset();
		std::cerr << "Cannot enable file logging: " << error.what() << '\n';
	}
	
	try
	{
		const auto options = m5emulator::parseOptions(argc, argv);
		if (!options) return EXIT_SUCCESS;
		std::signal(SIGINT, m5emulator::onSignal);
		std::signal(SIGTERM, m5emulator::onSignal);
		m5emulator::Application application(*options);
		return application.run(m5emulator::stopSignal);
	}
	catch (const std::exception& error)
	{
		std::cerr << "Error: " << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
