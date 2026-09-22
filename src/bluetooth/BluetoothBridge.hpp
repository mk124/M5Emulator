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

#include <filesystem>
#include <memory>

namespace m5emulator::bluetooth {

class BluetoothBridge
{
	struct Impl;
	
public:
	explicit BluetoothBridge(const std::filesystem::path& socketPath);
	~BluetoothBridge();
	
	BluetoothBridge(const BluetoothBridge&) = delete;
	BluetoothBridge& operator=(const BluetoothBridge&) = delete;
	
	void poll();
	void reset();
	void setEnabled(bool enabled);
	
private:
	std::unique_ptr<Impl> _impl;
};

} // namespace m5emulator::bluetooth
