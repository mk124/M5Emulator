#!/usr/bin/env python3
# Copyright (C) 2026 MK124 and contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

"""Extract read-only NimBLE context locators from compiled ESP-IDF libbt archives."""

import argparse
from pathlib import Path

from generate_ble_signatures import ROOT, archive_objects, pattern, quoted


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archives", type=Path, nargs="+")
    parser.add_argument("--ar", default="xtensa-esp-elf-ar")
    parser.add_argument("--output", type=Path, default=ROOT / "qemu/esp32s3/bluetooth/NimbleSignatures.hpp")
    args = parser.parse_args()
    name = "ble_att_svr_find_by_handle"
    signatures = set()
    for archive in args.archives:
        objects, members = archive_objects(archive, {name: "Nimble"}, args.ar)
        obj = objects[members[name]]
        signature = pattern(obj, name, "Nimble", {})
        # Verify the complete load of a supported BLE_STATIC_TO_DYNAMIC list.
        code = signature["code"]
        if code[9:14] not in (b"\x88\x08\x22\x28\x10", b"\x88\x08\x22\x28\x12"):
            raise ValueError("Unsupported NimBLE context layout")
        symbols = obj.get_section_by_name(".symtab")
        literal = obj.get_section_by_name(".rela.literal." + name)
        entries = list(literal.iter_relocations()) if literal else []
        if len(entries) != 1:
            raise ValueError("Expected a single context relocation")
        target = symbols.get_symbol(entries[0]["r_info_sym"])
        if target.name != "ble_att_svr_ctx" and obj.get_section(target["st_shndx"]).name != ".bss.ble_att_svr_ctx":
            raise ValueError("Expected a single reference to the NimBLE context")
        offsets = [i for i in (3, 6) if code[i] == 0x81]
        if len(offsets) != 1 or signature["calls"] or signature["references"]:
            raise ValueError("Unexpected find-by-handle instructions")
        # The audited SDK os_mempool layouts differ by one 32-bit field per
        # pool. All instructions stay fixed except context.list's displacement.
        for list_offset in (64, 72):
            layout_code = code[:13] + bytes([list_offset // 4]) + code[14:]
            signatures.add((layout_code, bytes(signature["mask"]), offsets[0], list_offset))

    license_header = (ROOT / "qemu/esp32s3/bluetooth/BleHle.h").read_text().split("#pragma once")[0]
    lines = ["#pragma once", "", '#include "CodeSignature.hpp"', "", "#include <tuple>", "",
             "namespace m5emulator::ble {", "", "using namespace std::literals::string_view_literals;", "",
             "// Generated from ESP-IDF NimBLE ble_att_svr.c (Apache-2.0), not application code.",
             "// Both audited os_mempool layouts are emitted: context.list at 64 / 72.",
             "// The literal identifies ble_att_svr_ctx; no guest function is intercepted.",
             "// Source attribution and license: resources/esp32s3/LICENSE.",
             "static constexpr std::tuple<CodeSignature, uint16_t, uint16_t> NimbleSignatures[] {"]
    for code, mask, offset, list_offset in sorted(signatures):
        lines += ['\t{ { "ble_att_svr_find_by_handle", 0, false,',
                  "\t\t" + quoted(code) + ",", "\t\t" + quoted(mask) + f", {{}}, {{}} }}, {offset}, {list_offset} }},"]
    lines += ["};", "", "} // namespace m5emulator::ble", ""]
    args.output.write_text(license_header + "\n".join(lines))
    print(f"Generated {len(signatures)} NimBLE locators")


if __name__ == "__main__":
    main()
