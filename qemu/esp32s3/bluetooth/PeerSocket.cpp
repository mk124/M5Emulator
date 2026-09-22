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

#include "PeerSocket.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "BleBytes.hpp"

namespace m5emulator::ble {

static constexpr size_t MaxMessageBytes = 1033, MaxQueuedMessages = 64;

PeerSocket::~PeerSocket() { close(); }

PeerSocket::PeerSocket(PeerSocket&& other) noexcept { *this = std::move(other); }

PeerSocket& PeerSocket::operator=(PeerSocket&& other) noexcept
{
	if (this == &other) return *this;
	
	close();
	_fd = std::exchange(other._fd, -1);
	_opened = other._opened;
	_incoming = std::move(other._incoming);
	_outgoing = std::move(other._outgoing);
	_sentBytes = other._sentBytes;
	return *this;
}

void PeerSocket::open()
{
	if (_opened) return;
	_opened = true;
	const char* path = std::getenv("M5EMU_BLE_PEER_SOCKET");
	if (path == nullptr) return;
	
	sockaddr_un address {};
	address.sun_family = AF_UNIX;
	if (std::strlen(path) >= sizeof(address.sun_path))
	{
		std::fprintf(stderr, "BLE HLE: peer socket path is too long\n");
		return;
	}
	std::strcpy(address.sun_path, path);
	
	_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (_fd < 0 || ::connect(_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
	{
		std::fprintf(stderr, "BLE HLE: cannot connect to peer socket: %s\n", std::strerror(errno));
		close();
		return;
	}
	adopt(std::exchange(_fd, -1));
}

void PeerSocket::adopt(int fd)
{
	close();
	_fd = fd;
	_opened = true;
	if (fcntl(_fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(_fd, F_SETFD, FD_CLOEXEC) < 0)
	{
		std::fprintf(stderr, "BLE HLE: cannot configure peer socket: %s\n", std::strerror(errno));
		close();
		return;
	}
	#ifdef SO_NOSIGPIPE
	const int enabled = 1;
	setsockopt(_fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
	#endif
}

void PeerSocket::close()
{
	if (_fd >= 0) ::close(std::exchange(_fd, -1));
	_incoming.clear();
	_outgoing.clear();
	_sentBytes = 0;
}

void PeerSocket::flush()
{
	while (_fd >= 0 && !_outgoing.empty())
	{
		const auto& packet = _outgoing.front();
		#ifdef MSG_NOSIGNAL
		const int flags = MSG_NOSIGNAL;
		#else
		const int flags = 0;
		#endif
		const ssize_t written = ::send(_fd, packet.data() + _sentBytes, packet.size() - _sentBytes, flags);
		if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
		if (written < 0 && errno == EINTR) continue;
		if (written <= 0)
		{
			close();
			return;
		}
		
		_sentBytes += written;
		if (_sentBytes != packet.size()) return;
		_outgoing.pop_front();
		_sentBytes = 0;
	}
}

std::vector<uint8_t> PeerSocket::receive()
{
	while (_fd >= 0)
	{
		const size_t required = _incoming.size() < 2 ? 2 : 2 + readLe16(_incoming);
		if (required > MaxMessageBytes + 2 || (required != 2 && required < 11))
		{
			close();
			return {};
		}
		if (_incoming.size() == required && required != 2)
		{
			std::vector<uint8_t> packet(_incoming.begin() + 2, _incoming.end());
			_incoming.clear();
			return packet;
		}
		
		uint8_t bytes[MaxMessageBytes + 2];
		const ssize_t count = ::recv(_fd, bytes, required - _incoming.size(), 0);
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return {};
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0)
		{
			close();
			return {};
		}
		_incoming.insert(_incoming.end(), bytes, bytes + count);
	}
	return {};
}

void PeerSocket::send(PeerMessage message, uint64_t generation, std::span<const uint8_t> payload)
{
	if (_fd < 0) return;
	if (payload.size() + 9 > MaxMessageBytes) fail("oversized BLE peer message");
	if (_outgoing.size() == MaxQueuedMessages)
	{
		close();
		return;
	}
	
	std::vector<uint8_t> packet;
	appendLe16(packet, payload.size() + 9);
	packet.push_back(static_cast<uint8_t>(message));
	for (unsigned shift = 0; shift < 64; shift += 8) packet.push_back(generation >> shift);
	packet.insert(packet.end(), payload.begin(), payload.end());
	_outgoing.push_back(std::move(packet));
	flush();
}

} // namespace m5emulator::ble
