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
#include <iterator>
#include <utility>

#include "BleBytes.hpp"

namespace m5emulator::ble {

std::optional<std::vector<uint8_t>> BleController::connectionCommand(uint16_t opcode, std::span<const uint8_t> parameters)
{
	static constexpr std::pair<uint16_t, uint8_t> CommandLengths[] {
		{ 0x0406, 3 },
		{ 0x041D, 2 },
		{ 0x1405, 2 },
		{ 0x2013, 14 },
		{ 0x2016, 2 },
		{ 0x201A, 18 },
		{ 0x201B, 2 },
		{ 0x2022, 6 },
		{ 0x2023, 0 },
		{ 0x2024, 4 },
		{ 0x202F, 0 },
		{ 0x2030, 2 },
		{ 0x2031, 3 },
		{ 0x2032, 7 }
	};
	const auto entry = std::ranges::find(CommandLengths, opcode, &std::pair<uint16_t, uint8_t>::first);
	if (entry == std::end(CommandLengths)) return std::nullopt;
	if (parameters.size() != entry->second) return std::vector<uint8_t> { 0x12 };
	if (opcode == 0x2023) return std::vector<uint8_t> { 0, 0xFB, 0, 0x48, 0x08 };
	if (opcode == 0x202F) return std::vector<uint8_t> { 0, 0xFB, 0, 0x48, 0x08, 0xFB, 0, 0x48, 0x08 };
	if (opcode == 0x2024) return std::vector<uint8_t> { 0x11 }; // Default DLE updates are not modeled.
	if (opcode == 0x2031) return std::vector<uint8_t> { 0x11 }; // Per-connection PHY selection is modeled below.
	
	const uint16_t handle = readLe16(parameters);
	std::vector<uint8_t> result { static_cast<uint8_t>(_connection && handle == _connection ? 0 : 2) };
	appendLe16(result, handle);
	if (result[0]) return result;
	
	switch (opcode)
	{
		case 0x0406: {
			disconnect(parameters[2]);
			break;
		}
		case 0x041D: {
			std::vector<uint8_t> event = result;
			event.insert(event.end(), { 9, 0xFF, 0xFF, 0, 0 });
			queueEvent(0x0C, event);
			break;
		}
		case 0x1405: {
			result.push_back(static_cast<uint8_t>(-45));
			break;
		}
		case 0x2016: {
			std::vector<uint8_t> event = result;
			event.insert(event.end(), { 0x63, 0x01, 0, 0, 0, 0, 0, 0 });
			queueLeEvent(0x04, event);
			break;
		}
		case 0x2013: {
			const uint16_t minimum = readLe16(parameters.subspan(2)), maximum = readLe16(parameters.subspan(4));
			const uint16_t latency = readLe16(parameters.subspan(6)), timeout = readLe16(parameters.subspan(8));
			if (minimum < 6 || minimum > maximum || maximum > 3200 || latency > 499 || timeout < 10 || timeout > 3200 || timeout * 4u <= (latency + 1u) * maximum) return std::vector<uint8_t> { 0x12 };
			
			_interval = minimum;
			_latency = latency;
			_supervisionTimeout = timeout;
			
			std::vector<uint8_t> event = result;
			appendLe16(event, _interval);
			appendLe16(event, _latency);
			appendLe16(event, _supervisionTimeout);
			queueLeEvent(0x03, event);
			break;
		}
		case 0x201A: {
			if (!_pendingKey)
			{
				result[0] = 0x0C;
				break;
			}
			
			uint8_t difference = 0;
			for (size_t i = 0; i < _pendingKey->size(); ++i) difference |= (*_pendingKey)[i] ^ parameters[2 + i];
			finishEncryption(difference == 0 ? 0 : 0x05);
			break;
		}
		case 0x201B: {
			if (!_pendingKey)
			{
				result[0] = 0x0C;
				break;
			}
			finishEncryption(0x06);
			break;
		}
		case 0x2022: {
			const uint16_t octets = readLe16(parameters.subspan(2)), timeUs = readLe16(parameters.subspan(4));
			if (octets < 27 || octets > 251 || timeUs < 328 || timeUs > 17040)
			{
				result[0] = 0x12;
				break;
			}
			
			std::vector<uint8_t> event;
			appendLe16(event, handle);
			appendLe16(event, octets);
			appendLe16(event, timeUs);
			appendLe16(event, 251);
			appendLe16(event, 2120);
			queueLeEvent(0x07, event);
			break;
		}
		case 0x2030: {
			result.insert(result.end(), { _phy, _phy });
			break;
		}
		case 0x2032: {
			if (parameters[2] > 3 || parameters[3] > 7 || parameters[4] > 7 || readLe16(parameters.subspan(5)) > 2) return std::vector<uint8_t> { 0x12 };
			const unsigned common = (parameters[2] & 1 ? 3 : parameters[3]) & (parameters[2] & 2 ? 3 : parameters[4]);
			if (!(common & 3)) return std::vector<uint8_t> { 0x11 };
			
			_phy = common & 2 ? 2 : 1;
			std::vector<uint8_t> event = result;
			event.insert(event.end(), { _phy, _phy });
			queueLeEvent(0x0C, event);
			break;
		}
	}
	return result;
}

} // namespace m5emulator::ble
