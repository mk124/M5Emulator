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
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

"""Check power-button IRQ delivery through the real PMIC and ESP32-S3 GPIO."""

import argparse
from pathlib import Path
import struct

from qemu_test import QemuTest, ROOT
from shared_state import INPUT_OFFSET


def expect(label, observed, expected):
    if observed != expected:
        raise AssertionError(f"{label}: got {observed!r}, expected {expected!r}")


def configure(qemu):
    qemu.write_register(0x6E, 0x10, 2)
    qemu.write_register(0x6E, 0x16, 4)
    qemu.write_registers(0x6E, 0x43, (0x1F, 0x3F, 6))
    qemu.write_registers(0x6E, 0x40, (0, 0, 0))
    qemu.write_register(0x6E, 0x49, 0x2F)  # Single reset disabled, single delay 1 s.
    qemu.write(0x600040A4, 0x2100)  # GPIO12 falling edge, CPU0 interrupt enabled.
    qemu.write(0x6000404C, 1 << 12)


def pin_high(qemu):
    return bool(qemu.read(0x6000403C) & (1 << 12))


def button(qemu, pressed, milliseconds):
    qemu.write_shared("=I", INPUT_OFFSET, 4 if pressed else 0)
    qemu.advance(milliseconds * 1000000)


def click_reaches_firmware_and_requires_acknowledgement(qemu):
    configure(qemu)
    expect("Fixed half-full battery voltage", struct.unpack("<H", qemu.read_registers(0x6E, 0x22, 2))[0], 3750)
    expect("External supply connected", struct.unpack("<H", qemu.read_registers(0x6E, 0x24, 2))[0], 5000)
    expect("Battery and USB source flags agree", qemu.read_register(0x6E, 0x04) & 5, 5)
    button(qemu, True, 50)
    expect("Press alone is not a click", pin_high(qemu), True)
    button(qemu, False, 900)
    expect("Configured click delay has not elapsed", pin_high(qemu), True)
    qemu.advance(150000000)
    expect("Click reaches GPIO12 without an I2C read", pin_high(qemu), False)
    expect("ESP32 GPIO interrupt is pending", bool(qemu.read(0x60004044) & (1 << 12)), True)
    expect("PMIC single-click status", qemu.read_register(0x6E, 0x42), 1)
    expect("Reading IRQ status does not acknowledge it", pin_high(qemu), False)
    qemu.write_register(0x6E, 0x42, 0xFF)
    expect("Writing ones preserves pending status", pin_high(qemu), False)
    qemu.write_register(0x6E, 0x42, 0xFE)
    expect("Writing zero acknowledges only the click", pin_high(qemu), True)
    print("PASS fixed power state and delayed power-button IRQ/acknowledgement", flush=True)


def masks_routing_and_mcu_reset_preserve_pending_external_events(qemu):
    configure(qemu)
    qemu.write_register(0x6E, 0x45, 7)
    button(qemu, True, 40)
    button(qemu, False, 1100)
    expect("Masked click cannot assert the host IRQ", pin_high(qemu), True)
    qemu.write_register(0x6E, 0x45, 6)
    expect("Unmasking exposes the pending click", pin_high(qemu), False)
    qemu.qmp("system_reset")
    expect("MCU reset cannot release an external PMIC IRQ", pin_high(qemu), False)
    expect("MCU reset preserves PMIC status", qemu.read_register(0x6E, 0x42), 1)
    qemu.write_register(0x6E, 0x16, 0)
    expect("Removing the last IRQ output discards status", qemu.read_register(0x6E, 0x42), 0)
    button(qemu, True, 40)
    button(qemu, False, 1100)
    qemu.write_register(0x6E, 0x16, 4)
    expect("No stale event appears when IRQ output is restored", pin_high(qemu), True)
    print("PASS masks, GPIO function gating and MCU-reset preservation", flush=True)


def long_and_double_presses_do_not_turn_into_single_clicks(qemu):
    configure(qemu)
    button(qemu, True, 2100)
    button(qemu, False, 1100)
    expect("Long press does not become a click on release", qemu.read_register(0x6E, 0x42), 0)
    qemu.write_register(0x6E, 0x4A, 1)  # Disable double-click shutdown to expose its IRQ.
    qemu.write_register(0x6E, 0x45, 2)
    button(qemu, True, 40)
    button(qemu, False, 100)
    button(qemu, True, 40)
    button(qemu, False, 1100)
    expect("Double click replaces its single-click candidate", qemu.read_register(0x6E, 0x42), 4)
    qemu.write_register(0x6E, 0x42, 0)
    qemu.write_register(0x6E, 0x49, 0x61)  # Double-click window exceeds single delay.
    button(qemu, True, 40)
    button(qemu, False, 400)
    expect("A longer double-click window defers the single event", pin_high(qemu), True)
    button(qemu, True, 40)
    button(qemu, False, 1100)
    expect("The configured double-click window remains effective", qemu.read_register(0x6E, 0x42), 4)
    qemu.write_register(0x6E, 0x42, 0)
    qemu.write_register(0x6E, 0x49, 0x2F)
    button(qemu, True, 40)
    button(qemu, False, 400)
    button(qemu, True, 40)
    button(qemu, False, 100)
    qemu.advance(500000000)
    expect("A separate click cannot erase the earlier candidate", qemu.read_register(0x6E, 0x42), 1)
    qemu.write_register(0x6E, 0x42, 0)
    qemu.advance(550000000)
    expect("Second independent click remains observable", qemu.read_register(0x6E, 0x42), 1)
    qemu.write_register(0x6E, 0x42, 0)
    qemu.write_register(0x6E, 0x49, 0x2E)
    button(qemu, True, 40)
    button(qemu, False, 1100)
    expect("Reset mode must not impersonate a firmware click", qemu.read_register(0x6E, 0x42), 0)
    print("PASS long/double-click separation and reset-mode gating", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=ROOT / ".deps/qemu/build/qemu-system-xtensa")
    args = parser.parse_args()
    with QemuTest(args.flash, args.qemu) as qemu:
        click_reaches_firmware_and_requires_acknowledgement(qemu)
        masks_routing_and_mcu_reset_preserve_pending_external_events(qemu)
        long_and_double_presses_do_not_turn_into_single_clicks(qemu)


if __name__ == "__main__":
    main()
