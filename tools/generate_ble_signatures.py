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

"""Generate relocatable S3 library signatures from ESP-IDF archives, never an application ELF.

Requires pyelftools and the Xtensa binutils shipped with ESP-IDF. This is a
maintainer tool: running the emulator and building it need neither ESP-IDF nor ELF files.
"""

import argparse
import hashlib
import io
import re
import struct
import subprocess
from pathlib import Path

from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
LIBRARIES = {
    "controller": "components/bt/controller/lib_esp32c3_family/esp32s3/libbtdm_app.a",
    "phy": "components/esp_phy/lib/esp32s3/libphy.a",
}
FUNCTIONS = {
    "controller": {
        "btdm_controller_init": "Init",
        "btdm_controller_deinit": "Deinit",
        "btdm_controller_enable": "Enable",
        "btdm_controller_disable": "Disable",
        "btdm_controller_get_mode": "Mode",
        "btdm_controller_get_compile_version": "Version",
        "API_vhci_host_check_send_available": "Available",
        "API_vhci_host_send_packet": "Send",
        "API_vhci_host_register_callback": "Register",
        "btdm_controller_enable_sleep": "SleepEnable",
        "btdm_controller_get_sleep_mode": "SleepMode",
        "btdm_wakeup_request": "Wakeup",
        "btdm_in_wakeup_requesting_set": "WakeupRequesting",
        "ble_txpwr_set": "PowerSet",
        "ble_txpwr_get": "PowerGet",
        "btdm_power_state_active": "PowerActive",
        "btdm_vnd_offload_task_register": "OffloadRegister",
        "btdm_vnd_offload_task_deregister": "OffloadDeregister",
        "sdk_config_extend_set_pll_track": "PllTrack",
        "scan_stack_enableAdvFlowCtrlVsCmd": "AdvFlowControl",
        "adv_stack_enableClearLegacyAdvVsCmd": "ClearLegacyAdv",
        "advFilter_stack_enableDupExcListVsCmd": "DuplicateExceptions",
        "chanSel_stack_enableSetCsaVsCmd": "ChannelSelection",
    },
    "phy": {
        "register_chipv7_phy": "PhyInit",
        "phy_wakeup_init": "PhyWakeup",
        "phy_close_rf": "PhyClose",
        "phy_xpd_tsens": "PhyTemperatureOff",
        "get_phy_version_str": "PhyVersion",
        "ram_phy_close_rf": "PhyCloseImpl",
        "ram_phy_wakeup_init": "PhyWakeupImpl",
        "phy_param_track_tot": "PhyTrack",
    },
}


def archive_objects(archive, functions, ar, optional=()):
    nm = str(Path(ar).with_name(Path(ar).name.removesuffix("-ar") + "-nm"))
    lines = subprocess.check_output([nm, "-A", "--defined-only", str(archive)],
                                    stderr=subprocess.DEVNULL, text=True).splitlines()
    members = {}
    for line in lines:
        fields = line.split()
        if len(fields) >= 3 and fields[-2] in ("T", "t") and fields[-1] in functions:
            members[fields[-1]] = line.split(":")[-2]
    if missing := functions.keys() - members.keys() - set(optional):
        raise ValueError(f"Missing library functions: {sorted(missing)}")
    objects = {member: ELFFile(io.BytesIO(subprocess.check_output([ar, "p", str(archive), member])))
               for member in set(members.values())}
    return objects, members


def pattern(obj, name, operation, rom):
    symbols = obj.get_section_by_name(".symtab")
    symbol = symbols.get_symbol_by_name(name)[0]
    section = obj.get_section(symbol["st_shndx"])
    iram = section.name.startswith(".iram")
    start, size = symbol["st_value"], symbol["st_size"]
    code = section.data()[start:start + size]
    mask = bytearray([255] * len(code))
    relocations, calls = {}, []
    for section in obj.iter_sections():
        if section["sh_type"] != "SHT_RELA":
            continue
        entries = list(section.iter_relocations())
        # R_XTENSA_ASM_EXPAND marks an L32R/CALLX8 pair which ld can relax to CALL8.
        relocations[section["sh_info"]] = {r["r_offset"]: r for r in entries if r["r_info_type"] != 11}
        if section["sh_info"] == symbol["st_shndx"]:
            calls = [r["r_offset"] - start for r in entries
                     if r["r_info_type"] == 11 and start <= r["r_offset"] < start + size]

    # Enough instructions to identify the routine without following arbitrary control flow.
    minimum = 48 if operation.startswith("Phy") or operation == "PowerActive" else 0
    limit = min([len(code), 192] + [offset for offset in calls if offset >= minimum])
    length = 0
    while length < limit:
        instruction_size = 6 if length in calls else (2 if code[length] & 15 >= 8 else 3)
        if length + instruction_size > limit:
            break
        length += instruction_size
    if length < 8 or code[:1] != b"\x36":
        raise ValueError(f"Not a usable windowed entry: {name}")

    references = []
    for address, relocation in relocations.get(symbol["st_shndx"], {}).items():
        offset = address - start
        if not 0 <= offset < length:
            continue
        kind = relocation["r_info_type"]
        if kind == 20 and code[offset] & 15 == 1:  # R_XTENSA_SLOT0_OP, L32R
            mask[offset + 1:offset + 3] = bytes(2)
            target = symbols.get_symbol(relocation["r_info_sym"])
            literal_offset = target["st_value"] + relocation["r_addend"]
            if not isinstance(target["st_shndx"], int):
                raise ValueError(f"External literal pool in {name}: {target.name}")
            literal = obj.get_section(target["st_shndx"]).data()
            word = struct.unpack_from("<I", literal, literal_offset)[0]
            reference = relocations.get(target["st_shndx"], {}).get(literal_offset)
            if reference is None:
                references.append({"offset": offset, "value": word})
                continue
            target = symbols.get_symbol(reference["r_info_sym"])
            # R_XTENSA_32 adds the literal's existing value even for an ELF RELA entry.
            addend = reference["r_addend"] + word
            if target.name in rom and target.name.startswith(("btdm_", "r_", "g_")):
                references.append({"offset": offset, "value": rom[target.name] + addend})
            elif any(target.name in functions for functions in FUNCTIONS.values()):
                if addend:
                    raise ValueError(f"Non-entry function reference in {name}: {target.name}")
                references.append({"offset": offset, "symbol": target.name})
            elif isinstance(target["st_shndx"], int):
                target_section = obj.get_section(target["st_shndx"])
                if target_section.name.startswith(".rodata"):
                    blob = target_section.data()
                    at = target["st_value"] + addend
                    end = blob.find(b"\0", at)
                    if end - at >= 6:
                        references.append({"offset": offset, "text": blob[at:min(end + 1, at + 96)]})
        elif kind == 20:
            # Keep branch opcode/register bits; the linker may move its destination.
            opcode = code[offset]
            if opcode & 15 == 7 or opcode & 0x3F == 0x26:
                mask[offset + 2] = 0
            elif opcode & 0x3F == 6:
                mask[offset] = 0x3F
                mask[offset + 1:offset + 3] = bytes(2)
            elif opcode & 15 == 6:
                mask[offset + 1:offset + 3] = b"\x0F\0"
            elif opcode & 15 == 0xC:
                mask[offset:offset + 2] = b"\x8F\x0F"
            else:
                raise ValueError(f"Unhandled SLOT0 relocation in {name} at {offset}: {opcode:02X}")
        elif kind == 1:  # R_XTENSA_32
            mask[offset:offset + 4] = bytes(4)
        else:
            raise ValueError(f"Unhandled relocation in {name}: {kind}")
    return {"name": name, "operation": operation, "iram": iram, "code": code[:length], "mask": mask[:length],
            "references": references, "calls": [offset for offset in calls if offset < length]}


def quoted(data):
    return '"' + "".join(f"\\x{byte:02X}" for byte in data) + '"sv'


def write_header(output, patterns, archives):
    license_header = (ROOT / "qemu/esp32s3/bluetooth/BleHle.h").read_text().split("#pragma once")[0]
    lines = ["#pragma once", "", '#include "CodeSignature.hpp"', '#include "BleHle.h"', "",
             "#include <array>", "", "namespace m5emulator::ble {", "",
             "using namespace std::literals::string_view_literals;", "",
             "// Generated by tools/generate_ble_signatures.py from Espressif's Apache-2.0 libraries.",
             "// Source attribution and license: resources/esp32s3/LICENSE."]
    lines += [f"// {name}: SHA-256 {hashlib.sha256(path.read_bytes()).hexdigest()}" for name, path in archives.items()]
    for index, signature in enumerate(patterns):
        name = signature["operation"] + str(index)
        calls, references = signature["calls"], signature["references"]
        lines.append(f"static constexpr std::array<uint16_t, {len(calls)}> {name}Calls {{ " +
                     ", ".join(str(offset) for offset in calls) + " };")
        lines.append(f"static constexpr std::array<CodeReference, {len(references)}> {name}References {{{{")
        for reference in references:
            value = reference.get("value", 0)
            text = quoted(reference.get("text", b""))
            symbol = reference.get("symbol", "")
            lines.append(f'\t{{ {reference["offset"]}, 0x{value:08X}, {text}, "{symbol}" }},')
        lines.append("}};")
    lines += ["", "static constexpr CodeSignature ControllerSignatures[] {"]
    for index, signature in enumerate(patterns):
        name = signature["operation"] + str(index)
        lines += [f'\t{{ "{signature["name"]}", Hle{signature["operation"]}, {str(signature["iram"]).lower()},',
                  "\t\t" + quoted(signature["code"]) + ",",
                  "\t\t" + quoted(signature["mask"]) + f", {name}Calls, {name}References }},"]
    lines += ["};", "", "} // namespace m5emulator::ble", ""]
    output.write_text(license_header + "\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, action="append", required=True, help="SDK source tree; repeat to include compatible library versions")
    parser.add_argument("--ar", default="xtensa-esp-elf-ar")
    parser.add_argument("--output", type=Path, default=ROOT / "qemu/esp32s3/bluetooth/ControllerSignatures.hpp")
    args = parser.parse_args()
    archives, patterns = {}, []
    optional = {"scan_stack_enableAdvFlowCtrlVsCmd", "adv_stack_enableClearLegacyAdvVsCmd",
                "advFilter_stack_enableDupExcListVsCmd", "chanSel_stack_enableSetCsaVsCmd"}
    for index, idf in enumerate(args.idf):
        rom = {}
        for path in (idf / "components/esp_rom/esp32s3/ld").glob("*.ld"):
            rom.update({name: int(value, 16) for name, value in re.findall(r"\b(\w+)\s*=\s*(0x[0-9a-fA-F]+)", path.read_text())})
        if not rom:
            parser.error(f"ESP32-S3 ROM exports not found in {idf}")
        for library, relative in LIBRARIES.items():
            archive = idf / relative
            archives[f"{library}{index}"] = archive
            objects, members = archive_objects(archive, FUNCTIONS[library], args.ar, optional)
            patterns += [pattern(objects[members[name]], name, operation, rom)
                         for name, operation in FUNCTIONS[library].items() if name in members]
    write_header(args.output, patterns, archives)
    print(f"Generated {len(patterns)} library signatures in {args.output}")


if __name__ == "__main__":
    main()
