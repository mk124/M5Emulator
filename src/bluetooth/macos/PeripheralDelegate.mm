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

#include "PeripheralDelegate.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <string>
#include <utility>

using namespace m5emulator::bluetooth;

static constexpr size_t MaxPendingOperations = 64;
static constexpr size_t MaxPendingValues = 64;
static constexpr auto UnsubscribedSessionTimeout = std::chrono::seconds(30);

@implementation M5PeripheralDelegate

- (instancetype)initWithPeer:(VirtualPeer*)peer
{
	if ((self = [super init]))
	{
		_peer = peer;
		_enabled = YES;
		_characteristics = [NSMutableDictionary dictionary];
		_services = [NSMutableArray array];
		_manager = [[CBPeripheralManager alloc] initWithDelegate:self queue:nil options:@{ CBPeripheralManagerOptionShowPowerAlertKey: @NO }];
	}
	return self;
}

- (void)shutdown
{
	_manager.delegate = nil;
	[self invalidate];
	_peer = nullptr;
	_manager = nil;
}

- (void)reset
{
	_peer->setEnabled(_enabled && _manager.state == CBManagerStatePoweredOn);
}

- (void)setEnabled:(BOOL)enabled
{
	_enabled = enabled;
	_peer->setEnabled(enabled && _manager.state == CBManagerStatePoweredOn);
	if (!enabled) [self invalidate];
	else if (_peer->available()) [self publish];
}

- (void)poll
{
	// CoreBluetooth has no disconnect callback for a central that only reads.
	if (_central && !_preparing && _desiredSubscriptions.empty() &&
	    std::chrono::steady_clock::now() - _lastActivity >= UnsubscribedSessionTimeout)
	{
		std::cout << "BLE radio: unsubscribed session expired; waiting for client activity\n" << std::flush;
		_peer->disconnect();
	}
}

- (void)invalidate
{
	++_publication;
	_published = NO;
	_pendingServices = 0;
	if (_manager.state == CBManagerStatePoweredOn)
	{
		[_manager stopAdvertising];
		[_manager removeAllServices];
	}
	
	[_characteristics removeAllObjects];
	[_services removeAllObjects];
	
	_central = nil;
	_preparing = NO;
	_desiredSubscriptions.clear();
	_subscriptions.clear();
	_values.clear();
	
	auto pending = std::move(_operations);
	_operations.clear();
	for (auto& operation : pending) operation.cancel(0x0E);
}

- (void)peripheralManagerDidUpdateState:(CBPeripheralManager*)manager
{
	if (_peer == nullptr || manager != _manager) return;
	
	std::cout << "BLE radio: CoreBluetooth state=" << static_cast<long>(manager.state)
	          << " authorization=" << static_cast<long>(CBManager.authorization) << '\n' << std::flush;
	if (manager.state == CBManagerStatePoweredOn && _enabled)
	{
		_peer->setEnabled(true);
		if (_peer->available()) [self publish];
	}
	else
	{
		_peer->setEnabled(false);
		[self invalidate];
		if (manager.state == CBManagerStateUnauthorized) std::cerr << "BLE radio: allow M5 Emulator in macOS Privacy & Security > Bluetooth\n";
		if (manager.state == CBManagerStateUnsupported) std::cerr << "BLE radio: peripheral role is unavailable on this Mac\n";
	}
}

- (void)updateAvailability
{
	if (_peer->available()) [self publish];
	else [self invalidate];
}

- (void)publish
{
	if (!_enabled || !_peer->available() || _manager.state != CBManagerStatePoweredOn || _published || _pendingServices) return;
	
	++_publication;
	for (const auto& service : _peer->services())
	{
		// macOS owns the GAP and GATT services.
		if (service.uuid == "1800" || service.uuid == "1801") continue;
		
		CBUUID* uuid = [CBUUID UUIDWithString:[NSString stringWithUTF8String:service.uuid.c_str()]];
		CBMutableService* published = [[CBMutableService alloc] initWithType:uuid primary:YES];
		NSMutableArray<CBMutableCharacteristic*>* characteristics = [NSMutableArray array];
		for (const auto& characteristic : service.characteristics)
		{
			CBCharacteristicProperties properties = characteristic.properties;
			for (const auto& descriptor : characteristic.descriptors)
			{
				if (descriptor.uuid != "2902" || !(descriptor.permissions & GattCharacteristic::WriteEncrypted)) continue;
				if (properties & CBCharacteristicPropertyNotify) properties |= CBCharacteristicPropertyNotifyEncryptionRequired;
				if (properties & CBCharacteristicPropertyIndicate) properties |= CBCharacteristicPropertyIndicateEncryptionRequired;
			}
			CBAttributePermissions permissions = 0;
			if (characteristic.permissions & GattCharacteristic::Read) permissions |= CBAttributePermissionsReadable;
			if (characteristic.permissions & GattCharacteristic::Write) permissions |= CBAttributePermissionsWriteable;
			if (characteristic.permissions & GattCharacteristic::ReadEncrypted) permissions |= CBAttributePermissionsReadEncryptionRequired;
			if (characteristic.permissions & GattCharacteristic::WriteEncrypted) permissions |= CBAttributePermissionsWriteEncryptionRequired;
			
			CBUUID* characteristicUuid = [CBUUID UUIDWithString:[NSString stringWithUTF8String:characteristic.uuid.c_str()]];
			CBMutableCharacteristic* value = [[CBMutableCharacteristic alloc] initWithType:characteristicUuid properties:properties value:nil permissions:permissions];
			_characteristics[@(characteristic.handle)] = value;
			[characteristics addObject:value];
		}
		published.characteristics = characteristics;
		[_services addObject:published];
	}
	
	_pendingServices = _services.count;
	for (CBMutableService* service in _services) [_manager addService:service];
}

- (void)peripheralManager:(CBPeripheralManager*)manager
            didAddService:(CBService*)service
                    error:(NSError*)error
{
	NSUInteger index = NSNotFound;
	for (NSUInteger i = 0; i < _services.count; ++i)
	{
		if (_services[i] == service) index = i;
	}
	if (index == NSNotFound) return;
	
	if (error)
	{
		std::cerr << "BLE radio: service " << service.UUID.UUIDString.UTF8String << " publication failed: " << error.localizedDescription.UTF8String << '\n';
		const bool advertised = std::ranges::find(_peer->advertisedServices(), std::string(service.UUID.UUIDString.UTF8String)) != _peer->advertisedServices().end();
		if (error.code != CBErrorUUIDNotAllowed || advertised)
		{
			_peer->setEnabled(false);
			[self invalidate];
			return;
		}
		
		// macOS reserves some standard services. Keep accepted application services usable.
		for (NSNumber* handle in _characteristics.allKeys)
		{
			if ([service.characteristics containsObject:_characteristics[handle]]) [_characteristics removeObjectForKey:handle];
		}
		[_services removeObjectAtIndex:index];
	}
	
	if (--_pendingServices) return;
	_published = YES;
	
	NSMutableArray<CBUUID*>* advertised = [NSMutableArray array];
	for (const auto& uuid : _peer->advertisedServices())
	{
		CBUUID* value = [CBUUID UUIDWithString:[NSString stringWithUTF8String:uuid.c_str()]];
		for (CBMutableService* published in _services)
		{
			if ([published.UUID isEqual:value]) [advertised addObject:value];
		}
	}
	if (!advertised.count)
	{
		std::cerr << "BLE radio: no advertised firmware service was published\n";
		_peer->setEnabled(false);
		return;
	}
	
	// Reserve advertisement space for discovery. UUIDs moved into Apple's overflow
	// area cannot be discovered by Android through normal service UUID filtering.
	std::cout << "BLE radio: requesting advertisement, service UUIDs=";
	for (CBUUID* uuid in advertised) std::cout << ' ' << uuid.UUIDString.UTF8String;
	std::cout << " (local name omitted)\n" << std::flush;
	[manager startAdvertising:@{ CBAdvertisementDataServiceUUIDsKey: advertised }];
}

- (void)peripheralManagerDidStartAdvertising:(CBPeripheralManager*)manager error:(NSError*)error
{
	if (error) std::cerr << "BLE radio: advertising failed: " << error.localizedDescription.UTF8String << '\n';
	else if (_published && manager.isAdvertising) std::cout << "BLE radio: CoreBluetooth accepted advertisement (isAdvertising=true)\n" << std::flush;
}

- (uint16_t)handleForCharacteristic:(CBCharacteristic*)characteristic
{
	for (NSNumber* handle in _characteristics)
	{
		if (_characteristics[handle] == characteristic) return handle.unsignedShortValue;
	}
	return 0;
}

- (uint16_t)cccdForHandle:(uint16_t)handle
{
	if (_peer == nullptr || !handle) return 0;
	
	for (const auto& service : _peer->services())
	{
		for (const auto& characteristic : service.characteristics)
		{
			if (characteristic.handle != handle) continue;
			
			for (const auto& descriptor : characteristic.descriptors)
			{
				if (descriptor.uuid == "2902") return descriptor.handle;
			}
		}
	}
	return 0;
}

- (void)enqueue:(PeripheralOperation&&)operation central:(CBCentral*)central
{
	if (!_published || !_peer->available() || (_central && ![_central.identifier isEqual:central.identifier]))
	{
		operation.cancel(0x0E);
		return;
	}
	if (_operations.size() >= MaxPendingOperations || central.maximumUpdateValueLength < DefaultAttMtu - 3U)
	{
		operation.cancel(0x11);
		return;
	}
	
	_operations.push_back(std::move(operation));
	_lastActivity = std::chrono::steady_clock::now();
	if (!_central)
	{
		_central = central;
		_preparing = YES;
		// Pairing may follow the first subscription. Keep the advertising session
		// available so macOS can route the pairing request to this application.
		if (!_peer->startSession())
		{
			[self invalidate];
			return;
		}
		if (_peer->ready()) [self prepareSession];
		return;
	}
	if (!_preparing) [self drainOperations];
}

- (void)prepareSession
{
	if (!_central || !_preparing || !_peer->ready()) return;
	
	const uint64_t publication = _publication;
	__weak M5PeripheralDelegate* observer = self;
	const uint16_t mtu = std::min<NSUInteger>(MaxAttMtu, _central.maximumUpdateValueLength + 3);
	_peer->exchangeMtu(mtu, [observer, publication] (uint8_t error, ByteView) {
		M5PeripheralDelegate* delegate = observer;
		if (!delegate || delegate->_publication != publication) return;
		if (error)
		{
			delegate->_peer->disconnect();
			return;
		}
		
		delegate->_preparing = NO;
		std::cout << "BLE radio: central bound; virtual ATT MTU=" << delegate->_peer->mtu() << '\n' << std::flush;
		[delegate drainOperations];
	});
}

- (void)drainOperations
{
	while (!_preparing && !_operations.empty())
	{
		auto operation = std::move(_operations.front());
		_operations.pop_front();
		operation.run();
	}
}

- (BOOL)acceptValue:(ByteView)value handle:(uint16_t)handle
{
	if (!_central || !_desiredSubscriptions.contains(handle)) return YES;
	if (!_characteristics[@(handle)]) return NO;
	if (_values.size() >= MaxPendingValues || value.size() > _central.maximumUpdateValueLength)
	{
		std::cerr << "BLE radio: notification queue/length limit exceeded; ending virtual session\n";
		return NO;
	}
	
	_values.push_back({ handle, Bytes(value.begin(), value.end()) });
	[self drainValues];
	return YES;
}

- (void)drainValues
{
	while (_central && !_preparing && !_values.empty())
	{
		const auto& entry = _values.front();
		if (!_subscriptions.contains(entry.handle)) return;
		
		NSData* value = [NSData dataWithBytes:entry.value.data() length:entry.value.size()];
		if (![_manager updateValue:value forCharacteristic:_characteristics[@(entry.handle)] onSubscribedCentrals:@[ _central ]]) return;
		_values.pop_front();
	}
}

- (void)peripheralManagerIsReadyToUpdateSubscribers:(CBPeripheralManager*)manager
{
	(void)manager;
	[self drainValues];
}

@end
