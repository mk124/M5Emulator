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

namespace m5emulator::ble {

using namespace std::literals::string_view_literals;

// NimBLE-Arduino 2.5.x ble_att_svr_find_by_handle, static-list loop form.
// ENTRY; narrow handle; L32R list; next; return if null; load handle;
// loop if unequal; RETW. Only the relocated L32R displacement is masked.
// This verifies the full traversal and handle offset, not an application address.
// Source attribution and license: resources/esp32s3/LICENSE.
static constexpr CodeSignature NimbleStaticListSignature {
	"ble_att_svr_find_by_handle", 0, false,
	"\x36\x41\x00\x20\x80\xF4\x21\x00\x00\x28\x02\x16\x52\x00\x92\x12\x05\x87\x99\xF4\x1D\xF0"sv,
	"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"sv,
	{}, {}
};

} // namespace m5emulator::ble
