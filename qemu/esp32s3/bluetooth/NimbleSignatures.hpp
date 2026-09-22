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

#include "CodeSignature.hpp"

#include <tuple>

namespace m5emulator::ble {

using namespace std::literals::string_view_literals;

// Generated from ESP-IDF NimBLE ble_att_svr.c (Apache-2.0), not application code.
// Both audited os_mempool layouts are emitted: context.list at 64 / 72.
// The literal identifies ble_att_svr_ctx; no guest function is intercepted.
// Source attribution and license: resources/esp32s3/LICENSE.
static constexpr std::tuple<CodeSignature, uint16_t, uint16_t> NimbleSignatures[] {
	{ { "ble_att_svr_find_by_handle", 0, false,
		"\x36\x41\x00\x20\x90\xF4\x81\x00\x00\x88\x08\x22\x28\x10\xC6\x01\x00\x82\x12\x05\x97\x18\x04\x28\x02\x56\x42\xFF\x1D\xF0"sv,
		"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00\x00\xFF\xFF\xFF\xFF\xFF\x3F\x00\x00\xFF\xFF\xFF\xFF\xFF\x00\xFF\xFF\xFF\x0F\x00\xFF\xFF"sv, {}, {} }, 6, 64 },
	{ { "ble_att_svr_find_by_handle", 0, false,
		"\x36\x41\x00\x20\x90\xF4\x81\x00\x00\x88\x08\x22\x28\x12\xC6\x01\x00\x82\x12\x05\x97\x18\x04\x28\x02\x56\x42\xFF\x1D\xF0"sv,
		"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00\x00\xFF\xFF\xFF\xFF\xFF\x3F\x00\x00\xFF\xFF\xFF\xFF\xFF\x00\xFF\xFF\xFF\x0F\x00\xFF\xFF"sv, {}, {} }, 6, 72 },
	{ { "ble_att_svr_find_by_handle", 0, false,
		"\x36\x41\x00\x81\x00\x00\x20\x90\xF4\x88\x08\x22\x28\x10\x16\xA2\x00\x82\x12\x05\x97\x18\x04\x28\x02\x56\x42\xFF\x1D\xF0"sv,
		"\xFF\xFF\xFF\xFF\x00\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x0F\x00\xFF\xFF\xFF\xFF\xFF\x00\xFF\xFF\xFF\x0F\x00\xFF\xFF"sv, {}, {} }, 3, 64 },
	{ { "ble_att_svr_find_by_handle", 0, false,
		"\x36\x41\x00\x81\x00\x00\x20\x90\xF4\x88\x08\x22\x28\x12\x16\xA2\x00\x82\x12\x05\x97\x18\x04\x28\x02\x56\x42\xFF\x1D\xF0"sv,
		"\xFF\xFF\xFF\xFF\x00\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x0F\x00\xFF\xFF\xFF\xFF\xFF\x00\xFF\xFF\xFF\x0F\x00\xFF\xFF"sv, {}, {} }, 3, 72 },
};

} // namespace m5emulator::ble
