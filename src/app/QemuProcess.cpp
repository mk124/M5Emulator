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

#include "QemuProcess.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "models/Device.hpp"
#include "Options.hpp"

extern char** environ;

namespace m5emulator {

namespace fs = std::filesystem;

static constexpr int ShutdownPollCount = 50;
static constexpr auto ShutdownPollInterval = std::chrono::milliseconds(10);

static constexpr int RecordingReceiveBufferBytes = 262144;

QemuProcess::~QemuProcess() { stop(); }

void QemuProcess::start(const Options& options, const fs::path& flashPath, const fs::path& sharedPath)
{
	std::string drive = "file=";
	for (const char c : flashPath.string())
	{
		drive += c;
		if (c == ',') drive += ',';
	}
	drive += ",if=mtd,format=raw";
	
	const auto& device = findDevice(options.deviceId);
	std::vector<std::string> args {
		options.qemuPath, "-L", options.qemuBiosPath,
		"-machine", std::string(device.qemuMachine),
		"-accel", (options.icount || options.controllerHle) ? "tcg,thread=single" : "tcg,thread=multi",
		"-display", "none", "-monitor", "none", "-serial", "stdio",
		"-audiodev", options.audio ? "sdl,id=audio,out.buffer-count=32" : "none,id=audio",
		"-drive", drive
	};
	for (const auto argument : device.qemuArguments) args.emplace_back(argument);
	if (!device.audioDevice.empty())
	{
		args.insert(args.end(), { "-global", std::string(device.audioDevice) + ".audiodev=audio", "-global", std::string(device.audioDevice) + (options.microphone ? ".microphone=on" : ".microphone=off") });
	}
	if (options.icount) args.insert(args.end(), { "-icount", "shift=auto" });
	
	std::vector<char*> argv;
	for (std::string& arg : args) argv.push_back(arg.data());
	argv.push_back(nullptr);
	
	std::vector<std::string> env;
	for (char** entry = environ; *entry != nullptr; ++entry)
	{
		const std::string_view variable = *entry;
		if (variable.starts_with("M5EMU_RECORDING_FD=") || variable.starts_with(std::string(device.sharedEnvironment) + "=") || variable.starts_with("M5EMU_BLE_HLE=") || variable.starts_with("M5EMU_BLE_PEER_SOCKET=")) continue;
		env.emplace_back(*entry);
	}
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, _recordingSocket.data()) < 0) throw std::system_error(errno, std::generic_category(), "recording socket");
	for (const int fd : _recordingSocket)
	{
		if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) throw std::system_error(errno, std::generic_category(), "configure recording socket");
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &RecordingReceiveBufferBytes, sizeof(RecordingReceiveBufferBytes));
	}
	// The child does not use the host endpoint. Replace it with its own
	// endpoint so dup2 also clears CLOEXEC on macOS.
	const int recordingFd = _recordingSocket[0];
	env.push_back("M5EMU_RECORDING_FD=" + std::to_string(recordingFd));
	env.push_back(std::string(device.sharedEnvironment) + "=" + sharedPath.string());
	if (options.controllerHle) env.emplace_back("M5EMU_BLE_HLE=1");
	if (!options.blePeerSocket.empty()) env.push_back("M5EMU_BLE_PEER_SOCKET=" + options.blePeerSocket.string());
	std::vector<char*> envp;
	for (std::string& entry : env) envp.push_back(entry.data());
	envp.push_back(nullptr);
	
	// Keep QEMU's console connected when our own stdin is already at EOF
	// (desktop launch, CI or redirected input), so UART output survives.
	for (auto* descriptors : { &_consolePipe, &_outputPipe })
	{
		if (pipe(descriptors->data()) < 0) throw std::system_error(errno, std::generic_category(), "console pipe");
		for (const int fd : *descriptors)
		{
			if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) throw std::system_error(errno, std::generic_category(), "console close-on-exec");
		}
	}
	if (fcntl(_outputPipe[0], F_SETFL, O_NONBLOCK) < 0) throw std::system_error(errno, std::generic_category(), "console nonblocking read");
	
	posix_spawn_file_actions_t actions;
	int error = posix_spawn_file_actions_init(&actions);
	if (error) throw std::system_error(error, std::generic_category(), "spawn actions");
	error = posix_spawn_file_actions_adddup2(&actions, _recordingSocket[1], recordingFd);
	if (!error) error = posix_spawn_file_actions_adddup2(&actions, _consolePipe[0], STDIN_FILENO);
	if (!error) error = posix_spawn_file_actions_adddup2(&actions, _outputPipe[1], STDOUT_FILENO);
	
	pid_t spawned = -1;
	if (!error) error = posix_spawnp(&spawned, options.qemuPath.c_str(), &actions, nullptr, argv.data(), envp.data());
	posix_spawn_file_actions_destroy(&actions);
	if (error) throw std::system_error(error, std::generic_category(), "launch QEMU " + options.qemuPath);
	
	close(_consolePipe[0]);
	_consolePipe[0] = -1;
	close(_outputPipe[1]);
	_outputPipe[1] = -1;
	close(_recordingSocket[1]);
	_recordingSocket[1] = -1;
	_pid = spawned;
}

bool QemuProcess::exited(int& status)
{
	drainOutput();
	if (_pid < 0) return false;
	
	pid_t result;
	do { result = waitpid(_pid, &status, WNOHANG); } while (result < 0 && errno == EINTR);
	if (result < 0) throw std::system_error(errno, std::generic_category(), "waitpid");
	if (!result) return false;
	
	_pid = -1;
	return true;
}

void QemuProcess::stop() noexcept
{
	if (_pid >= 0) kill(_pid, SIGTERM);
	for (int& fd : _consolePipe)
	{
		if (fd >= 0) close(fd);
		fd = -1;
	}
	
	for (int i = 0; _pid >= 0 && i < ShutdownPollCount; ++i)
	{
		drainOutput();
		const pid_t result = waitpid(_pid, nullptr, WNOHANG);
		if (result == _pid || (result < 0 && errno == ECHILD))
		{
			_pid = -1;
			break;
		}
		std::this_thread::sleep_for(ShutdownPollInterval);
	}
	
	if (_pid >= 0)
	{
		kill(_pid, SIGKILL);
		while (waitpid(_pid, nullptr, 0) < 0 && errno == EINTR) {}
		_pid = -1;
	}
	closeOutput();
	for (int& fd : _recordingSocket)
	{
		if (fd >= 0) close(fd);
		fd = -1;
	}
}

} // namespace m5emulator
