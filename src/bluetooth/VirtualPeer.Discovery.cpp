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

static Bytes discoveryRequest(uint8_t opcode, uint16_t start, uint16_t end, uint16_t type = 0)
{
	Bytes packet { opcode };
	appendLe16(packet, start);
	appendLe16(packet, end);
	if (type) appendLe16(packet, type);
	return packet;
}

void VirtualPeer::discoverServices(uint16_t start)
{
	request(discoveryRequest(0x10, start, 0xFFFF, 0x2800), [this, start] (uint8_t error, ByteView value) {
		if (!_generation) return;
		if (error == 0x0A)
		{
			require(!_discoveredServices.empty(), "original firmware has no GATT services");
			discoverCharacteristics(0, _discoveredServices[0].start);
			return;
		}
		
		require(!error && value.size() >= 7, "GATT service discovery failed");
		const size_t size = value[0];
		require((size == 6 || size == 20) && (value.size() - 1) % size == 0, "invalid GATT service list");
		
		uint16_t next = start;
		for (size_t i = 1; i < value.size(); i += size)
		{
			const ByteView entry = value.subspan(i, size);
			const uint16_t first = readLe16(entry), last = readLe16(entry.subspan(2));
			require(first >= next && last >= first && _discoveredServices.size() < MaxServices, "invalid GATT service range");
			_discoveredServices.push_back({ first, last, uuidString(entry.subspan(4)), {} });
			if (last == 0xFFFF)
			{
				discoverCharacteristics(0, _discoveredServices[0].start);
				return;
			}
			next = last + 1;
		}
		discoverServices(next);
	});
}

void VirtualPeer::discoverCharacteristics(size_t service, uint16_t start)
{
	if (service == _discoveredServices.size())
	{
		discoverDescriptors(0, 0, 0);
		return;
	}
	
	request(discoveryRequest(8, start, _discoveredServices[service].end, 0x2803), [this, service, start] (uint8_t error, ByteView value) {
		if (!_generation) return;
		
		const auto nextService = [&] {
			const size_t next = service + 1;
			discoverCharacteristics(next, next < _discoveredServices.size() ? _discoveredServices[next].start : 0);
		};
		if (error == 0x0A)
		{
			nextService();
			return;
		}
		
		require(!error && value.size() >= 8, "GATT characteristic discovery failed");
		const size_t size = value[0];
		require((size == 7 || size == 21) && (value.size() - 1) % size == 0, "invalid GATT characteristic list");
		auto& entry = _discoveredServices[service];
		
		uint16_t next = start;
		for (size_t i = 1; i < value.size(); i += size)
		{
			const ByteView characteristic = value.subspan(i, size);
			const uint16_t declaration = readLe16(characteristic), handle = readLe16(characteristic.subspan(3));
			require(declaration >= next && handle > declaration && handle <= entry.end && entry.characteristics.size() < MaxCharacteristics, "invalid GATT characteristic handle");
			entry.characteristics.push_back({ declaration, handle, characteristic[2], uuidString(characteristic.subspan(5)), {} });
			if (handle == entry.end)
			{
				nextService();
				return;
			}
			next = handle + 1;
		}
		discoverCharacteristics(service, next);
	});
}

void VirtualPeer::discoverDescriptors(size_t service, size_t characteristic, uint16_t start)
{
	while (service < _discoveredServices.size() && characteristic == _discoveredServices[service].characteristics.size())
	{
		++service;
		characteristic = 0;
	}
	if (service == _discoveredServices.size())
	{
		discoveryComplete();
		return;
	}
	
	const auto& group = _discoveredServices[service];
	const auto& entry = group.characteristics[characteristic];
	const uint16_t end = characteristic + 1 < group.characteristics.size() ? group.characteristics[characteristic + 1].declaration - 1 : group.end;
	if (entry.handle == end)
	{
		discoverDescriptors(service, characteristic + 1, 0);
		return;
	}
	if (!start) start = entry.handle + 1;
	request(discoveryRequest(4, start, end), [this, service, characteristic, start, end] (uint8_t error, ByteView value) {
		if (!_generation) return;
		if (error == 0x0A)
		{
			discoverDescriptors(service, characteristic + 1, 0);
			return;
		}
		
		require(!error && value.size() >= 5, "GATT descriptor discovery failed");
		require(value[0] == 1 || value[0] == 2, "invalid GATT descriptor format");
		const size_t size = value[0] == 1 ? 4 : 18;
		require((value.size() - 1) % size == 0, "invalid GATT descriptor list");
		auto& entry = _discoveredServices[service].characteristics[characteristic];
		
		uint16_t next = start;
		for (size_t i = 1; i < value.size(); i += size)
		{
			const uint16_t handle = readLe16(value.subspan(i));
			require(handle >= next && handle <= end && entry.descriptors.size() < MaxDescriptors, "invalid GATT descriptor handle");
			entry.descriptors.push_back({ handle, uuidString(value.subspan(i + 2, size - 2)) });
			if (handle == end)
			{
				discoverDescriptors(service, characteristic + 1, 0);
				return;
			}
			next = handle + 1;
		}
		discoverDescriptors(service, characteristic, next);
	});
}

void VirtualPeer::discoveryComplete()
{
	std::cout << "BLE bridge: discovered " << _discoveredServices.size() << " original GATT services\n";
	for (const auto& service : _discoveredServices)
	{
		std::cout << "BLE bridge: service " << service.uuid << " [" << service.start << ',' << service.end << "]\n";
		for (const auto& characteristic : service.characteristics)
		{
			std::cout << "BLE bridge: characteristic " << characteristic.uuid << " handle=" << characteristic.handle << " properties=" << static_cast<unsigned>(characteristic.properties) << '\n';
		}
	}
	std::cout << std::flush;
	
	// Permissions are absent from ATT discovery; compare the discoverable structure only.
	require(_services.size() == _discoveredServices.size(), "GATT service count changed since publication");
	for (size_t i = 0; i < _services.size(); ++i)
	{
		const auto& published = _services[i].characteristics;
		auto& discovered = _discoveredServices[i].characteristics;
		require(published.size() == discovered.size(), "GATT characteristic count changed since publication");
		for (size_t j = 0; j < published.size(); ++j)
		{
			discovered[j].permissions = published[j].permissions;
			discovered[j].minimumKeySize = published[j].minimumKeySize;
			require(published[j].descriptors.size() == discovered[j].descriptors.size(), "GATT descriptor count changed since publication");
			for (size_t k = 0; k < published[j].descriptors.size(); ++k)
			{
				discovered[j].descriptors[k].permissions = published[j].descriptors[k].permissions;
				discovered[j].descriptors[k].minimumKeySize = published[j].descriptors[k].minimumKeySize;
			}
		}
	}
	
	// Verify the published database through actual ATT before releasing queued client requests.
	require(_catalogReady && _sessionRequested && _services == _discoveredServices, "GATT database changed since publication");
	_discoveredServices.clear();
	clearSubscriptions(0, 0);
}

void VirtualPeer::clearSubscriptions(size_t service, size_t characteristic)
{
	for (; service < _services.size(); ++service, characteristic = 0)
	{
		const auto& entries = _services[service].characteristics;
		for (; characteristic < entries.size(); ++characteristic)
		{
			for (const auto& descriptor : entries[characteristic].descriptors)
			{
				if (descriptor.uuid != "2902") continue;
				
				Bytes packet { 0x12 };
				appendLe16(packet, descriptor.handle);
				appendLe16(packet, 0);
				request(std::move(packet), [this, service, characteristic] (uint8_t error, ByteView) {
					if (!_generation) return;
					require(!error, "could not clear restored subscriptions for the new session");
					clearSubscriptions(service, characteristic + 1);
				});
				return;
			}
		}
	}
	
	_ready = true;
	if (onReady) onReady();
}

} // namespace m5emulator::bluetooth
