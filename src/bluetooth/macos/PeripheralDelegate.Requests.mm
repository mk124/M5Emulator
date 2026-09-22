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
#include <array>
#include <iostream>
#include <utility>

using namespace m5emulator::bluetooth;

@implementation M5PeripheralDelegate (Requests)

- (void)peripheralManager:(CBPeripheralManager*)manager didReceiveReadRequest:(CBATTRequest*)request
{
	const uint16_t handle = [self handleForCharacteristic:request.characteristic];
	if (!handle || request.offset > 0xFFFF || !(request.characteristic.properties & CBCharacteristicPropertyRead))
	{
		[manager respondToRequest:request withResult:!handle ? CBATTErrorAttributeNotFound : request.offset > 0xFFFF ? CBATTErrorInvalidOffset : CBATTErrorReadNotPermitted];
		return;
	}
	
	__weak M5PeripheralDelegate* observer = self;
	const uint64_t publication = _publication;
	PeripheralOperation operation {
		[observer, manager, request, handle, publication] {
			M5PeripheralDelegate* delegate = observer;
			if (!delegate || delegate->_publication != publication)
			{
				[manager respondToRequest:request withResult:CBATTErrorUnlikelyError];
				return;
			}
			
			delegate->_peer->read(handle, request.offset, [manager, request] (uint8_t error, ByteView value) {
				if (!error) request.value = [NSData dataWithBytes:value.data() length:value.size()];
				[manager respondToRequest:request withResult:static_cast<CBATTError>(error)];
			});
		},
		[manager, request] (uint8_t error) { [manager respondToRequest:request withResult:static_cast<CBATTError>(error)]; }
	};
	[self enqueue:std::move(operation) central:request.central];
}

- (void)peripheralManager:(CBPeripheralManager*)manager didReceiveWriteRequests:(NSArray<CBATTRequest*>*)requests
{
	if (!requests.count) return;
	
	CBATTRequest* request = requests.firstObject;
	// The public callback hides the ATT opcode. Multi-request atomic writes cannot be
	// rolled back after arbitrary guest callbacks, so reject the whole batch before execution.
	if (requests.count != 1)
	{
		std::cerr << "BLE radio: atomic multi-request write is unsupported\n";
		[manager respondToRequest:request withResult:CBATTErrorRequestNotSupported];
		return;
	}
	
	const uint16_t handle = [self handleForCharacteristic:request.characteristic];
	if (!handle || request.offset || !(request.characteristic.properties & (CBCharacteristicPropertyWrite | CBCharacteristicPropertyWriteWithoutResponse)))
	{
		[manager respondToRequest:request withResult:!handle ? CBATTErrorAttributeNotFound : request.offset ? CBATTErrorInvalidOffset : CBATTErrorWriteNotPermitted];
		return;
	}
	
	__weak M5PeripheralDelegate* observer = self;
	const uint64_t publication = _publication;
	PeripheralOperation operation {
		[observer, manager, request, handle, publication] {
			M5PeripheralDelegate* delegate = observer;
			if (!delegate || delegate->_publication != publication)
			{
				[manager respondToRequest:request withResult:CBATTErrorUnlikelyError];
				return;
			}
			
			const ByteView value(static_cast<const uint8_t*>(request.value.bytes), request.value.length);
			delegate->_peer->write(handle, value, [manager, request, handle, size = value.size()] (uint8_t error, ByteView) {
				if (error) std::cerr << "BLE radio: guest write rejected; handle=" << handle << " bytes=" << size << " ATT error=" << static_cast<unsigned>(error) << '\n';
				[manager respondToRequest:request withResult:static_cast<CBATTError>(error)];
			});
		},
		[manager, request] (uint8_t error) { [manager respondToRequest:request withResult:static_cast<CBATTError>(error)]; }
	};
	[self enqueue:std::move(operation) central:request.central];
}

- (void)    peripheralManager:(CBPeripheralManager*)manager
                      central:(CBCentral*)central
 didSubscribeToCharacteristic:(CBCharacteristic*)characteristic
{
	(void)manager;
	const uint16_t handle = [self handleForCharacteristic:characteristic];
	const uint16_t cccd = [self cccdForHandle:handle];
	if (!handle || !cccd || (_central && ![_central.identifier isEqual:central.identifier])) return;
	
	_desiredSubscriptions.insert(handle);
	const uint8_t bits = characteristic.properties & CBCharacteristicPropertyNotify ? 1 : 2;
	
	const uint64_t publication = _publication;
	__weak M5PeripheralDelegate* observer = self;
	PeripheralOperation operation {
		[observer, publication, handle, cccd, bits] {
			M5PeripheralDelegate* delegate = observer;
			if (!delegate || delegate->_publication != publication || !delegate->_desiredSubscriptions.contains(handle)) return;
			
			delegate->_peer->write(cccd, std::array<uint8_t, 2> { bits, 0 }, [observer, publication, handle] (uint8_t error, ByteView) {
				M5PeripheralDelegate* delegate = observer;
				if (!delegate || delegate->_publication != publication) return;
				if (error)
				{
					delegate->_peer->disconnect();
					return;
				}
				
				if (delegate->_desiredSubscriptions.contains(handle)) delegate->_subscriptions.insert(handle);
				std::cout << "BLE radio: subscribed to original handle " << handle << '\n' << std::flush;
				[delegate drainValues];
			});
		},
		[observer, publication, handle] (uint8_t) {
			M5PeripheralDelegate* delegate = observer;
			if (delegate && delegate->_publication == publication) delegate->_desiredSubscriptions.erase(handle);
		}
	};
	[self enqueue:std::move(operation) central:central];
}

- (void)        peripheralManager:(CBPeripheralManager*)manager
                          central:(CBCentral*)central
 didUnsubscribeFromCharacteristic:(CBCharacteristic*)characteristic
{
	(void)manager;
	const uint16_t handle = [self handleForCharacteristic:characteristic];
	if (!handle || !_central || ![_central.identifier isEqual:central.identifier]) return;
	
	const uint16_t cccd = [self cccdForHandle:handle];
	_desiredSubscriptions.erase(handle);
	_subscriptions.erase(handle);
	std::erase_if(_values, [handle] (const PeripheralValue& value) { return value.handle == handle; });
	if (_desiredSubscriptions.empty())
	{
		std::cout << "BLE radio: final subscription ended; ending virtual session\n" << std::flush;
		_peer->disconnect();
		return;
	}
	
	const uint64_t publication = _publication;
	__weak M5PeripheralDelegate* observer = self;
	PeripheralOperation operation {
		[observer, publication, cccd] {
			M5PeripheralDelegate* delegate = observer;
			if (!delegate || delegate->_publication != publication) return;
			
			delegate->_peer->write(cccd, std::array<uint8_t, 2> {}, [observer, publication] (uint8_t error, ByteView) {
				M5PeripheralDelegate* delegate = observer;
				if (error && delegate && delegate->_publication == publication) delegate->_peer->disconnect();
			});
		},
		[] (uint8_t) {}
	};
	[self enqueue:std::move(operation) central:central];
}

@end
