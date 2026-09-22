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

#include <chrono>
#include <cstdint>
#include <deque>
#include <set>

#import <CoreBluetooth/CoreBluetooth.h>

#include "../BleBytes.hpp"
#include "../VirtualPeer.hpp"
#include "PeripheralOperation.hpp"
#include "PeripheralValue.hpp"

@interface M5PeripheralDelegate : NSObject <CBPeripheralManagerDelegate>
{
	m5emulator::bluetooth::VirtualPeer* _peer;
	CBPeripheralManager* _manager;
	BOOL _enabled;
	
	NSMutableDictionary<NSNumber*, CBMutableCharacteristic*>* _characteristics;
	NSMutableArray<CBMutableService*>* _services;
	NSUInteger _pendingServices;
	uint64_t _publication;
	BOOL _published;
	
	CBCentral* _central;
	BOOL _preparing;
	std::chrono::steady_clock::time_point _lastActivity;
	std::deque<m5emulator::bluetooth::PeripheralOperation> _operations;
	
	std::set<uint16_t> _desiredSubscriptions, _subscriptions;
	std::deque<m5emulator::bluetooth::PeripheralValue> _values;
}

- (instancetype)initWithPeer:(m5emulator::bluetooth::VirtualPeer*)peer;
- (void)shutdown;

- (void)reset;
- (void)setEnabled:(BOOL)enabled;
- (void)poll;
- (void)invalidate;

- (void)updateAvailability;
- (void)publish;

- (uint16_t)handleForCharacteristic:(CBCharacteristic*)characteristic;
- (uint16_t)cccdForHandle:(uint16_t)handle;

- (void)enqueue:(m5emulator::bluetooth::PeripheralOperation&&)operation central:(CBCentral*)central;
- (void)prepareSession;
- (void)drainOperations;

- (BOOL)acceptValue:(m5emulator::bluetooth::ByteView)value handle:(uint16_t)handle;
- (void)drainValues;

@end
