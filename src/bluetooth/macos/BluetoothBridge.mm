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

#include "../BluetoothBridge.hpp"

#include "PeripheralDelegate.hpp"

namespace m5emulator::bluetooth {

static constexpr int MaxRunLoopSourcesPerPoll = 8;

struct BluetoothBridge::Impl
{
	explicit Impl(const std::filesystem::path& socketPath) : peer(socketPath) {}
	
	VirtualPeer peer;
	M5PeripheralDelegate* delegate = nil;
};

BluetoothBridge::BluetoothBridge(const std::filesystem::path& socketPath) : _impl(std::make_unique<Impl>(socketPath))
{
	_impl->peer.setEnabled(false);
	_impl->delegate = [[M5PeripheralDelegate alloc] initWithPeer:&_impl->peer];
	
	__weak M5PeripheralDelegate* observer = _impl->delegate;
	_impl->peer.onAvailabilityChanged = [observer] { [observer updateAvailability]; };
	_impl->peer.onReady = [observer] { [observer prepareSession]; };
	_impl->peer.onClosed = [observer] { [observer invalidate]; };
	_impl->peer.onValue = [observer] (uint16_t handle, ByteView value, bool) { return static_cast<bool>([observer acceptValue:value handle:handle]); };
}

BluetoothBridge::~BluetoothBridge()
{
	_impl->peer.reset();
	_impl->peer.onReady = _impl->peer.onClosed = _impl->peer.onAvailabilityChanged = {};
	_impl->peer.onValue = {};
	
	[_impl->delegate shutdown];
}

void BluetoothBridge::poll()
{
	@autoreleasepool
	{
		// CoreBluetooth and SDL share the main thread; also pump callbacks in headless mode.
		for (int i = 0; i < MaxRunLoopSourcesPerPoll; ++i)
		{
			if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true) != kCFRunLoopRunHandledSource) break;
		}
		
		_impl->peer.poll();
		[_impl->delegate poll];
	}
}

void BluetoothBridge::reset()
{
	_impl->peer.reset();
	[_impl->delegate reset];
}

void BluetoothBridge::setEnabled(bool enabled)
{
	[_impl->delegate setEnabled:enabled];
}

} // namespace m5emulator::bluetooth
