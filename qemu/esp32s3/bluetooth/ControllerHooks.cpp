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

#include "BleHle.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ControllerSignatures.hpp"
#include "NimbleSignatures.hpp"
#include "NimbleStaticSignature.hpp"

namespace m5emulator::ble {

static std::map<uint32_t, uint32_t> hooks;

static uint32_t nimbleContextPointer, nimbleListPointer, nimbleListOffset;

static std::map<std::string_view, std::vector<uint32_t>> symbols;

// GNU ld may choose the density form of these instructions without changing semantics.
static uint32_t instruction(std::span<const uint8_t> code, size_t& length)
{
	if (code.size() < 2)
	{
		length = 0;
		return 0;
	}
	const uint32_t a = code[0], b = code[1];
	length = (a & 15) >= 8 ? 2 : 3;
	if (code.size() < length)
	{
		length = 0;
		return 0;
	}
	if (a == 0x1D && b == 0xF0) return 0x90; // RETW.N / RETW
	if (a == 0x0D && b == 0xF0) return 0x80; // RET.N / RET
	if (a == 0x3D && b == 0xF0) return 0x20F0; // NOP.N / NOP
	if ((a & 15) == 8 || (a & 15) == 9)
	{
		return ((a & 0xF0) | 2) | (((a & 15) == 8 ? 0x20 : 0x60) | (b & 15)) << 8 | (b >> 4) << 16;
	}
	if ((a & 15) == 0xD && (b & 0xF0) == 0)
	{
		return (b << 4) | ((a & 0xF0) | b) << 8 | 0x200000;
	}
	if ((a & 0x8F) == 0x0C)
	{
		int immediate = (a & 0x70) | (b >> 4);
		if (immediate >= 96) immediate -= 128;
		return ((b & 15) << 4 | 2) | (0xA0 | ((immediate >> 8) & 15)) << 8 | (immediate & 255) << 16;
	}
	if ((a & 0x8F) == 0x8C)
	{
		return (a & 0x40 ? 0x56 : 0x16) | (b & 15) << 8;
	}
	if ((a & 15) == 0xB)
	{
		const uint32_t immediate = a >> 4 ? a >> 4 : 255;
		return ((b & 0xF0) | 2) | (0xC0 | (b & 15)) << 8 | immediate << 16;
	}
	return a | b << 8 | (length == 3 ? code[2] << 16 : 0);
}

static bool readLiteral(void* guest, uint32_t pc, uint32_t& value)
{
	std::array<uint8_t, 3> code {};
	if (!bleHleTryRead(guest, pc, code.data(), code.size())) return false;
	const uint32_t literal = ((pc + 3) & ~3u) + (static_cast<int32_t>(code[1] | code[2] << 8) - 65536) * 4;
	return bleHleTryRead(guest, literal, &value, sizeof(value));
}

static bool referenceMatches(void* guest, uint32_t pc, uint32_t callTarget, const CodeReference& reference, bool verifySymbols)
{
	uint32_t value = callTarget;
	if (!value && !readLiteral(guest, pc, value)) return false;
	
	if (!reference.symbol.empty())
	{
		if (!verifySymbols) return true;
		const auto found = symbols.find(reference.symbol);
		return found != symbols.end() && found->second.size() == 1 && found->second.front() == value;
	}
	
	if (reference.bytes.empty()) return value == reference.value;
	
	std::array<char, 96> bytes {};
	return reference.bytes.size() <= bytes.size() && bleHleTryRead(guest, value, bytes.data(), reference.bytes.size()) &&
	       std::equal(reference.bytes.begin(), reference.bytes.end(), bytes.begin());
}

static bool matches(void* guest, uint32_t pc, std::span<const uint8_t> code, const CodeSignature& signature, bool verifySymbols = false)
{
	const auto* expected = reinterpret_cast<const uint8_t*>(signature.code.data());
	const auto* mask = reinterpret_cast<const uint8_t*>(signature.mask.data());
	size_t offset = 0, position = 0;
	while (offset < signature.code.size())
	{
		if (position + 6 > code.size()) return false;
		uint32_t callTarget = 0;
		size_t expectedLength = 0, actualLength = 0;
		if (std::ranges::find(signature.calls, offset) != signature.calls.end())
		{
			// A relaxed CALL8 can retain a padding instruction to preserve branch alignment.
			while (position + 3 <= code.size() && ((code[position] == 0x10 && code[position + 1] == 0x11 && code[position + 2] == 0x20) ||
			                                       (code[position] == 0xF0 && code[position + 1] == 0x20 && code[position + 2] == 0))) position += 3;
			if (position + 6 > code.size()) return false;
			if ((code[position] & 0x3F) == 0x25)
			{
				int32_t displacement = (code[position] | code[position + 1] << 8 | code[position + 2] << 16) >> 6;
				if (displacement & 0x20000) displacement -= 0x40000;
				callTarget = ((pc + position) & ~3u) + 4 + displacement * 4;
				actualLength = 3;
			}
			else
			{
				if (code[position] != expected[offset] || code[position + 3] != 0xE0 ||
				    code[position + 4] != expected[offset + 4] || code[position + 5] != 0) return false;
				actualLength = 6;
			}
			expectedLength = 6;
		}
		else
		{
			const uint32_t wanted = instruction({ expected + offset, signature.code.size() - offset }, expectedLength);
			const uint32_t actual = instruction(code.subspan(position), actualLength);
			if (!expectedLength || !actualLength) return false;
			uint32_t bits = mask[offset] | mask[offset + 1] << 8;
			if (expectedLength == 3) bits |= mask[offset + 2] << 16;
			else if ((expected[offset] & 0x8F) == 0x8C) bits = 0xFFF;
			else if (bits == 0xFFFF) bits = 0xFFFFFF;
			if ((wanted & bits) != (actual & bits)) return false;
		}
		
		for (const auto& reference : signature.references)
		{
			if (reference.offset == offset && !referenceMatches(guest, pc + position, callTarget, reference, verifySymbols)) return false;
		}
		offset += expectedLength;
		position += actualLength;
	}
	return true;
}

} // namespace m5emulator::ble

extern "C" void bleHleDiscover(void* guest)
{
	using namespace m5emulator::ble;
	if (!hooks.empty()) return;
	
	std::array<std::vector<uint32_t>, std::size(ControllerSignatures)> candidates;
	std::vector<std::pair<uint32_t, uint16_t>> nimbleContexts;
	std::vector<uint32_t> nimbleLists;
	for (const auto [base, size] : { std::pair { 0x40370000u, 0x70000u }, std::pair { 0x42000000u, 0x2000000u } })
	{
		std::vector<uint8_t> code(size);
		for (uint32_t offset = 0; offset < size; offset += 0x10000)
		{
			bleHleTryRead(guest, base + offset, code.data() + offset, std::min(0x10000u, size - offset));
		}
		for (size_t offset = 0; offset + 256 < code.size(); offset += 4)
		{
			if (code[offset] != 0x36 || (code[offset + 1] & 15) != 1) continue;
			if (base >= 0x42000000)
			{
				if (matches(guest, base + offset, std::span(code).subspan(offset, 256), NimbleStaticListSignature))
				{
					uint32_t address = 0;
					if (readLiteral(guest, base + offset + 6, address)) nimbleLists.push_back(address);
				}
				for (const auto& [signature, reference, listOffset] : NimbleSignatures)
				{
					if (!matches(guest, base + offset, std::span(code).subspan(offset, 256), signature)) continue;
					uint32_t address = 0;
					if (readLiteral(guest, base + offset + reference, address)) nimbleContexts.emplace_back(address, listOffset);
				}
			}
			for (size_t i = 0; i < std::size(ControllerSignatures); ++i)
			{
				const auto& signature = ControllerSignatures[i];
				// Flash-mapped data can contain the load image of an .iram section.
				if (signature.iram && base >= 0x42000000) continue;
				if (code[offset + 1] != static_cast<uint8_t>(signature.code[1]) || code[offset + 2] != static_cast<uint8_t>(signature.code[2])) continue;
				if (matches(guest, base + offset, std::span(code).subspan(offset, 256), signature)) candidates[i].push_back(base + offset);
			}
		}
	}
	
	const size_t databases = nimbleContexts.size() + nimbleLists.size();
	nimbleContextPointer = databases == 1 && !nimbleContexts.empty() ? nimbleContexts.front().first : 0;
	nimbleListOffset = nimbleContextPointer ? nimbleContexts.front().second : 0;
	nimbleListPointer = databases == 1 && !nimbleLists.empty() ? nimbleLists.front() : 0;
	std::fprintf(stderr, "BLE HLE: read-only NimBLE database candidates=%zu context=%08X list=%08X\n", databases, nimbleContextPointer, nimbleListPointer);
	
	for (size_t i = 0; i < std::size(ControllerSignatures); ++i)
	{
		auto& addresses = symbols[ControllerSignatures[i].name];
		addresses.insert(addresses.end(), candidates[i].begin(), candidates[i].end());
		std::ranges::sort(addresses);
		addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
	}
	
	std::map<std::string_view, std::set<uint32_t>> verified;
	for (size_t i = 0; i < std::size(ControllerSignatures); ++i)
	{
		const auto& signature = ControllerSignatures[i];
		std::erase_if(candidates[i], [&] (uint32_t address) {
			std::array<uint8_t, 256> code {};
			return !bleHleTryRead(guest, address, code.data(), code.size()) || !matches(guest, address, code, signature, true);
		});
		verified[signature.name].insert(candidates[i].begin(), candidates[i].end());
	}
	
	std::set<std::string_view> visited;
	for (const auto& signature : ControllerSignatures)
	{
		if (!visited.insert(signature.name).second) continue;
		const auto& addresses = verified[signature.name];
		std::fprintf(stderr, "BLE HLE: %s candidates=%zu", signature.name.data(), addresses.size());
		if (addresses.size() == 1)
		{
			const uint32_t address = *addresses.begin();
			std::fprintf(stderr, " entry=%08X", address);
			if (signature.operation != HleVersion && signature.operation != HlePhyVersion) hooks.emplace(address + 3, signature.operation);
		}
		if (addresses.size() > 1)
		{
			for (uint32_t address : addresses) std::fprintf(stderr, " %08X", address);
			std::fprintf(stderr, "\nBLE HLE: ambiguous library entry %s; refusing interception\n", signature.name.data());
			std::exit(1);
		}
		std::fputc('\n', stderr);
	}
	
	// Version-query routines may be removed with disabled logging. Require the
	// actual controller/PHY entries; init separately validates the config/OSI ABI.
	for (const uint32_t required : { HleInit, HleDeinit, HleEnable, HleDisable, HleMode, HleAvailable, HleSend, HleRegister, HlePowerActive, HlePhyInit, HlePhyWakeup, HlePhyClose })
	{
		if (std::ranges::none_of(hooks, [=] (const auto& entry) { return entry.second == required; }))
		{
			const auto signature = std::ranges::find(ControllerSignatures, required, &CodeSignature::operation);
			std::fprintf(stderr, "BLE HLE: unsupported controller ABI; missing %s\n", signature->name.data());
			std::exit(1);
		}
	}
}

extern "C" void bleHleResetHooks()
{
	m5emulator::ble::hooks.clear();
	m5emulator::ble::nimbleContextPointer = 0;
	m5emulator::ble::nimbleListPointer = 0;
	m5emulator::ble::nimbleListOffset = 0;
	m5emulator::ble::symbols.clear();
}

extern "C" uint32_t bleHleGattList(void* guest)
{
	using namespace m5emulator::ble;
	uint32_t list = nimbleListPointer;
	if (nimbleContextPointer)
	{
		uint32_t context = 0;
		if (!bleHleTryRead(guest, nimbleContextPointer, &context, sizeof(context)) || !context || context > UINT32_MAX - nimbleListOffset) return 0;
		list = context + nimbleListOffset; // Layout verified by the full SDK function signature.
	}
	
	uint32_t entry = 0;
	if (list) bleHleTryRead(guest, list, &entry, sizeof(entry));
	return entry;
}

extern "C" uint32_t bleHleOperation(uint32_t pc)
{
	const auto found = m5emulator::ble::hooks.find(pc);
	return found == m5emulator::ble::hooks.end() ? pc : found->second;
}

extern "C" bool bleHleHandles(uint32_t pc)
{
	if (!bleHleEnabled()) return false;
	return pc == HleRomDataInit || (pc >= HleReturn && pc <= HleDelayReturned && !(pc & 3)) ||
	       pc == HleTaskPoll || m5emulator::ble::hooks.contains(pc);
}
