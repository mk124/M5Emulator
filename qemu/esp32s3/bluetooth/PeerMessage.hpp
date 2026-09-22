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

#include <cstdint>

namespace m5emulator::ble {

// Private Unix transport: LE16 length, message byte, LE64 connection generation, payload.
enum class PeerMessage : uint8_t
{
	Connect = 1,
	Disconnect = 2,
	L2cap = 3,
	Encrypt = 4,
	Advertising = 0x80,
	Connected = 0x81,
	Disconnected = 0x82,
	Data = 0x83,
	Encryption = 0x84,
	Rejected = 0x85,
	Reset = 0x86,
	GattDatabase = 0x87
};

} // namespace m5emulator::ble
