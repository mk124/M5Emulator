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

#include "VirtualPeer.hpp"

#include <iostream>
#include <utility>

namespace m5emulator::bluetooth {

static constexpr size_t MaxCatalogSize = 32768;

void VirtualPeer::invalidateCatalog()
{
	_catalogReady = _catalogReceiving = false;
	_catalogBuffer.clear();
	_services.clear();
	_discoveredServices.clear();
	
	if (onAvailabilityChanged) onAvailabilityChanged();
}

void VirtualPeer::receiveCatalog(ByteView payload)
{
	require(!payload.empty() && !(payload[0] & ~3), "invalid GATT metadata chunk");
	const uint8_t flags = payload[0];
	if (flags & 1)
	{
		_catalogBuffer.clear();
		_catalogReceiving = true;
	}
	require(_catalogReceiving && _catalogBuffer.size() + payload.size() <= MaxCatalogSize, "invalid GATT metadata sequence/size");
	_catalogBuffer.insert(_catalogBuffer.end(), payload.begin() + 1, payload.end());
	if (!(flags & 2)) return;
	
	_catalogReceiving = false;
	require(!_catalogBuffer.empty(), "GATT publication unavailable for this NimBLE layout; no automatic connection attempted");
	
	std::vector<GattService> services;
	require(_catalogBuffer[0] == 2, "incompatible GATT metadata version");
	ByteView remaining = ByteView(_catalogBuffer).subspan(1);
	uint16_t previous = 0;
	while (!remaining.empty())
	{
		require(remaining.size() >= 6, "truncated GATT attribute metadata");
		const uint16_t handle = readLe16(remaining);
		const uint8_t permissions = remaining[2], minimumKeySize = remaining[3];
		const size_t uuidSize = remaining[4];
		require(handle > previous && remaining.size() >= 6 + uuidSize, "invalid GATT attribute metadata");
		const std::string uuid = uuidString(remaining.subspan(5, uuidSize));
		const size_t valueSize = remaining[5 + uuidSize];
		require(remaining.size() >= 6 + uuidSize + valueSize, "truncated GATT declaration metadata");
		const ByteView value = remaining.subspan(6 + uuidSize, valueSize);
		remaining = remaining.subspan(6 + uuidSize + valueSize);
		previous = handle;
		
		if (uuid == "2800")
		{
			require(services.size() < MaxServices, "too many GATT services");
			if (!services.empty()) services.back().end = handle - 1;
			services.push_back({ handle, 0xFFFF, uuidString(value), {} });
			continue;
		}
		require(!services.empty() && uuid != "2801", "unsupported GATT service layout");
		auto& characteristics = services.back().characteristics;
		if (uuid == "2803")
		{
			require(characteristics.size() < MaxCharacteristics && (value.size() == 5 || value.size() == 19), "invalid GATT characteristic metadata");
			const uint16_t valueHandle = readLe16(value.subspan(1));
			require(valueHandle > handle, "invalid GATT value handle");
			characteristics.push_back({ handle, valueHandle, value[0], uuidString(value.subspan(3)), {} });
		}
		else if (uuid != "2802")
		{
			require(!characteristics.empty(), "GATT attribute without a characteristic");
			auto& characteristic = characteristics.back();
			if (handle == characteristic.handle)
			{
				require(uuid == characteristic.uuid, "GATT value UUID mismatch");
				require(!(permissions & GattCharacteristic::AuthenticatedOrAuthorized), "GATT requires authenticated/authorized access that CoreBluetooth cannot enforce");
				require(minimumKeySize <= 16, "invalid GATT encryption key size");
				characteristic.permissions = permissions;
				characteristic.minimumKeySize = minimumKeySize;
			}
			else
			{
				require(handle > characteristic.handle && characteristic.descriptors.size() < MaxDescriptors, "invalid GATT descriptor metadata");
				require(!(permissions & GattCharacteristic::AuthenticatedOrAuthorized), "GATT descriptor requires authenticated/authorized access that CoreBluetooth cannot enforce");
				characteristic.descriptors.push_back({ handle, uuid, permissions, minimumKeySize });
			}
		}
	}
	
	require(!services.empty(), "empty GATT service metadata");
	const bool changed = !_catalogReady || services != _services;
	if (changed)
	{
		disconnect();
		invalidateCatalog();
		_services = std::move(services);
		_catalogReady = true;
		std::cout << "BLE bridge: published metadata for " << _services.size() << " original GATT services; awaiting client activity\n"
		          << std::flush;
	}
	
	_catalogBuffer.clear();
	if (onAvailabilityChanged) onAvailabilityChanged();
}

} // namespace m5emulator::bluetooth
