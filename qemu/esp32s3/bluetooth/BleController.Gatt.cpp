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

#include "BleController.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>

#include "BleBytes.hpp"

namespace m5emulator::ble {

static std::vector<uint8_t> readGattBytes(void* guest, uint32_t address, size_t size)
{
	std::vector<uint8_t> bytes(size);
	if (!address || address > UINT32_MAX - 32 || size > 32 || !bleHleTryRead(guest, address, bytes.data(), size)) throw std::runtime_error("invalid NimBLE GATT pointer");
	return bytes;
}

static std::vector<uint8_t> readGattUuid(void* guest, uint32_t address)
{
	const uint8_t type = readGattBytes(guest, address, 1)[0];
	if (type == 16) return readGattBytes(guest, address + 2, 2);
	if (type == 128) return readGattBytes(guest, address + 1, 16);
	if (type == 32)
	{
		const auto value = readGattBytes(guest, address + 4, 4);
		std::vector<uint8_t> uuid { 0xFB, 0x34, 0x9B, 0x5F, 0x80, 0, 0, 0x80, 0, 0x10, 0, 0 };
		uuid.insert(uuid.end(), value.begin(), value.end());
		return uuid;
	}
	throw std::runtime_error("invalid NimBLE UUID type");
}

void BleController::publishGatt(void* guest)
{
	try
	{
		uint32_t entry = bleHleGattList(guest);
		if (!entry) throw std::runtime_error("NimBLE GATT database unavailable or empty; automatic probe connections are disabled");
		
		// Shared ESP-IDF / NimBLE-Arduino ble_att_svr_entry layout:
		// next/uuid/flags/key-size/handle/callback/argument.
		std::vector<uint8_t> database { 2 };
		uint16_t previousHandle = 0;
		while (entry)
		{
			const auto attribute = readGattBytes(guest, entry, 20);
			const uint16_t handle = readLe16(std::span(attribute).subspan(10));
			if (handle <= previousHandle || database.size() > 32000) throw std::runtime_error("invalid NimBLE GATT list");
			previousHandle = handle;
			
			const auto uuid = readGattUuid(guest, readLe32(std::span(attribute).subspan(4)));
			const uint32_t argument = readLe32(std::span(attribute).subspan(16));
			const uint32_t next = readLe32(attribute);
			
			std::vector<uint8_t> value;
			if (uuid.size() == 2 && (readLe16(uuid) == 0x2800 || readLe16(uuid) == 0x2801))
			{
				value = readGattUuid(guest, readLe32(readGattBytes(guest, argument + 4, 4)));
			}
			else if (uuid.size() == 2 && readLe16(uuid) == 0x2803)
			{
				const auto definition = readGattBytes(guest, argument, 20);
				const auto characteristicUuid = readGattUuid(guest, readLe32(definition));
				const auto nextAttribute = readGattBytes(guest, next, 20);
				if (readGattUuid(guest, readLe32(std::span(nextAttribute).subspan(4))) != characteristicUuid) throw std::runtime_error("invalid NimBLE characteristic value attribute");
				const uint16_t flags = readLe16(std::span(definition).subspan(16));
				value.push_back((flags & 0x7F) | ((flags & 0x180) ? 0x80 : 0));
				appendLe16(value, readLe16(std::span(nextAttribute).subspan(10)));
				value.insert(value.end(), characteristicUuid.begin(), characteristicUuid.end());
			}
			
			appendLe16(database, handle);
			database.push_back(attribute[8]);
			database.push_back(attribute[9]);
			database.push_back(uuid.size());
			database.insert(database.end(), uuid.begin(), uuid.end());
			database.push_back(value.size());
			database.insert(database.end(), value.begin(), value.end());
			entry = next;
		}
		
		if (database.empty()) throw std::runtime_error("NimBLE GATT database is empty");
		// Each message fits the existing bounded transport. No read callback is executed.
		for (size_t offset = 0; offset < database.size(); offset += 1000)
		{
			const size_t size = std::min<size_t>(1000, database.size() - offset);
			std::vector<uint8_t> packet { static_cast<uint8_t>((offset == 0 ? 1 : 0) | (offset + size == database.size() ? 2 : 0)) };
			packet.insert(packet.end(), database.begin() + offset, database.begin() + offset + size);
			_peer.send(PeerMessage::GattDatabase, 0, packet);
		}
		std::fprintf(stderr, "BLE HLE: published read-only GATT metadata (%zu bytes), no connection created\n", database.size());
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "BLE HLE: GATT publication unavailable: %s\n", error.what());
		_peer.send(PeerMessage::GattDatabase, 0, std::array<uint8_t, 1> { 3 });
	}
}

} // namespace m5emulator::ble
