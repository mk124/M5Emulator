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

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "Screen.hpp"
#include "DeviceState.hpp"
#include "FlashLayout.hpp"

namespace m5emulator {

// Views and callbacks borrow static model definitions.
struct Device
{
	std::string_view id, name, label, qemuMachine;
	Screen screen;
	// Buttons use bits 0..N-1 and keys 1..N; power and Home masks are board-specific.
	std::span<const std::string_view> buttons;
	uint32_t powerButton, homeButtons;
	bool touch, motion, powerLed, vibration;
	
	FlashLayout flash;
	std::span<const std::string_view> qemuArguments;
	std::string_view audioDevice, sharedEnvironment;
	std::size_t sharedSize;
	void (*resetShared)(void* memory);
	void (*exchangeShared)(void* memory, DeviceState& state);
};

extern const std::array<Device, 1> Devices;

inline const Device& findDevice(std::string_view id)
{
	for (const auto& device : Devices)
	{
		if (device.id == id) return device;
	}
	throw std::runtime_error("Unsupported device: " + std::string(id));
}

} // namespace m5emulator
