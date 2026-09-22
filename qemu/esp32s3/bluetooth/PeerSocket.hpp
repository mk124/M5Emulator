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

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include "PeerMessage.hpp"

namespace m5emulator::ble {

class PeerSocket
{
public:
	PeerSocket() = default;
	~PeerSocket();
	
	PeerSocket(const PeerSocket&) = delete;
	PeerSocket& operator=(const PeerSocket&) = delete;
	PeerSocket(PeerSocket&& other) noexcept;
	PeerSocket& operator=(PeerSocket&& other) noexcept;
	
	void open();
	void adopt(int fd);
	void close();
	
	void flush();
	std::vector<uint8_t> receive();
	void send(PeerMessage message, uint64_t generation, std::span<const uint8_t> payload);
	
	bool connected() const { return _fd >= 0; }
	
private:
	int _fd = -1;
	bool _opened = false;
	
	std::vector<uint8_t> _incoming;
	
	std::deque<std::vector<uint8_t>> _outgoing;
	size_t _sentBytes = 0;
};

} // namespace m5emulator::ble
