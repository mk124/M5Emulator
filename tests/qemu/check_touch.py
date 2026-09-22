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

"""Check CST820 change interrupts through actual QEMU I2C, GPIO and RTC devices."""

import argparse
from pathlib import Path

from qemu_test import QemuTest, ROOT
from shared_state import INPUT_OFFSET

GPIO = 0x60004000
RTC = 0x60008000
TOUCH_PIN = 1 << 13


def expect(label, observed, expected):
    if observed != expected:
        raise AssertionError(f"{label}: got {observed!r}, expected {expected!r}")


def configure_touch(qemu):
    qemu.write_register(0x4F, 0x03, 0x08)  # M5IOE1 IO4 drives touch reset.
    qemu.write_register(0x4F, 0x05, 0)
    qemu.advance(1000000)
    qemu.write_register(0x4F, 0x05, 0x08)
    qemu.write_register(0x15, 0xFA, 0x20)
    qemu.write_register(0x15, 0xED, 20)  # 2 ms, as configured by M5GFX.
    qemu.advance(100000000)
    qemu.write(GPIO + 0x74 + 13 * 4, 0x2100)  # GPIO13 falling-edge interrupt.
    qemu.write(GPIO + 0x4C, TOUCH_PIN)


def touch(qemu, x, y, down):
    qemu.write_shared("=iiI", INPUT_OFFSET + 4, x, y, int(down))
    qemu.advance(1000000)  # Deliver input through the board's regular input poll.


def irq_level(qemu):
    return bool(qemu.read(GPIO + 0x3C) & TOUCH_PIN)


def change_interrupts_are_masked_pulses_not_held_levels(qemu):
    configure_touch(qemu)
    qemu.write_register(0x15, 0xFA, 0)
    touch(qemu, 100, 100, True)
    expect("Disabled mode keeps IRQ high", irq_level(qemu), True)
    expect("Disabled mode creates no GPIO edge", qemu.read(GPIO + 0x44) & TOUCH_PIN, 0)
    touch(qemu, 100, 100, False)
    qemu.write_register(0x15, 0xFA, 0x20)

    for label, x, down, pulse_width, before_end in (
        ("press", 100, True, 20, 1999999),
        ("move", 140, True, 40, 3999999),
        ("release", 140, False, 20, 1999999),
    ):
        qemu.write_register(0x15, 0xED, pulse_width)
        qemu.write(GPIO + 0x4C, TOUCH_PIN)
        touch(qemu, x, 100, down)
        expect(f"{label} asserts a low pulse", irq_level(qemu), False)
        expect(f"{label} produces a falling edge", qemu.read(GPIO + 0x44) & TOUCH_PIN, TOUCH_PIN)
        qemu.write(GPIO + 0x4C, TOUCH_PIN)
        for _ in range(3):
            qemu.read_registers(0x15, 0x02, 5)
        expect("I2C polling creates no new edge", qemu.read(GPIO + 0x44) & TOUCH_PIN, 0)
        qemu.advance(before_end)
        expect(f"{label} pulse lasts the configured width", irq_level(qemu), False)
        qemu.advance(1)
        expect(f"{label} pulse releases on time without a guest read", irq_level(qemu), True)
        qemu.advance(20000000)
        expect("Unchanged touch never retriggers", qemu.read(GPIO + 0x44) & TOUCH_PIN, 0)
        expect("A held finger does not hold IRQ low", irq_level(qemu), True)

    touch(qemu, 200, 120, False)
    expect("Hover movement creates no touch interrupt", qemu.read(GPIO + 0x44) & TOUCH_PIN, 0)
    touch(qemu, 200, 120, True)
    qemu.write_register(0x15, 0xFA, 0)
    expect("Masking cancels an active pulse", irq_level(qemu), True)
    qemu.write(GPIO + 0x4C, TOUCH_PIN)
    qemu.write_register(0x15, 0xFA, 0x20)
    expect("Enabling a mode does not invent a touch change", qemu.read(GPIO + 0x44) & TOUCH_PIN, 0)
    qemu.write_register(0x15, 0xED, 40)
    touch(qemu, 220, 120, True)
    qemu.advance(1999999)
    expect("Cancelled pulse cannot truncate a later pulse", irq_level(qemu), False)
    qemu.advance(2000001)
    expect("Replacement pulse ends at its own deadline", irq_level(qemu), True)

    touch(qemu, 240, 120, True)
    for x in (250, 260, 270):
        touch(qemu, x, 120, True)
    qemu.advance(999999)
    expect("Changes during a pulse do not end it early", irq_level(qemu), False)
    qemu.advance(1)
    expect("Continuous movement cannot extend IRQ indefinitely", irq_level(qemu), True)
    print("PASS touch press/move/release pulses, widths, read independence and masking", flush=True)


def touch_changes_wake_ext1_and_allow_sleep_while_held(qemu):
    configure_touch(qemu)
    qemu.write(RTC + 0xE0, TOUCH_PIN)
    for label, x, down in (("press", 100, True), ("move", 140, True), ("release", 140, False)):
        qemu.write(RTC + 0x4C, 3)
        qemu.write(RTC + 0x18, 2)
        qemu.write(RTC + 0x3C, 2 << 15)  # EXT1 only, ANY_LOW on GPIO13.
        qemu.write(RTC + 0x18, 1 << 31)
        expect(f"Sleep before {label} is accepted", bool(qemu.read(RTC + 0x18) & (1 << 31)), True)
        qemu.advance(20000000)
        expect("An unchanged finger does not wake the CPU", bool(qemu.read(RTC + 0x18) & (1 << 31)), True)
        touch(qemu, x, 100, down)
        expect(f"{label} wakes without polling the touch chip", bool(qemu.read(RTC + 0x18) & (1 << 31)), False)
        expect(f"{label} records EXT1 wake", qemu.read(RTC + 0x130), 2)
        expect(f"{label} records GPIO13", qemu.read(RTC + 0xE4), TOUCH_PIN)
        qemu.advance(2000000)
        expect("IRQ is released before the next sleep", irq_level(qemu), True)
    print("PASS touch changes wake through GPIO13/EXT1 and held input permits sleep", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=ROOT / ".deps/qemu/build/qemu-system-xtensa")
    args = parser.parse_args()
    for check in (change_interrupts_are_masked_pulses_not_held_levels, touch_changes_wake_ext1_and_allow_sleep_while_held):
        with QemuTest(args.flash, args.qemu) as qemu:
            check(qemu)


if __name__ == "__main__":
    main()
