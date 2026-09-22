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

"""Exercise digital GPIO and software I2C against the actual board chips.

Protects M5GFX autodetection from broken GPIO readback/pulls, incorrect
ACK/data sequencing, duplicate buses and reset-line/identity regressions.
"""

import argparse
from pathlib import Path

from qemu_test import QemuTest, ROOT

GPIO = 0x60004000
MUX = 0x60009000
SDA = 1 << 15  # GPIO47, upper register bank.
SCL = 1 << 16  # GPIO48.


def gpio_drive_release_and_pulls(qemu):
    # An output low must override the external I2C pullup; input releases it.
    qemu.write(GPIO + 0x18, SCL)
    qemu.write(GPIO + 0x30, SCL)
    assert qemu.read(GPIO + 0x40) & SCL == 0, "Driven low must be readable as low"
    qemu.write(GPIO + 0x34, SCL)
    qemu.write(MUX + 4 + 48 * 4, 0x80)
    assert qemu.read(GPIO + 0x40) & SCL, "Board pullup must dominate internal pulldown"
    # An unrelated unconnected pin must not acquire the board's I2C pullup.
    qemu.write(MUX + 4 + 10 * 4, 0x80)
    assert qemu.read(GPIO + 0x3C) & (1 << 10) == 0, "Floating pin follows pulldown"
    qemu.write(MUX + 4 + 10 * 4, 0x100)
    assert qemu.read(GPIO + 0x3C) & (1 << 10), "Floating pin follows pullup"
    # Open-drain high releases the pad, so a pulldown can bring it low.
    qemu.write(GPIO + 0x74 + 10 * 4, 4)
    qemu.write(GPIO + 8, 1 << 10)
    qemu.write(GPIO + 0x24, 1 << 10)
    qemu.write(MUX + 4 + 10 * 4, 0x80)
    assert qemu.read(GPIO + 0x3C) & (1 << 10) == 0, "Open-drain high must release, not drive high"
    qemu.write(GPIO + 0x74 + 10 * 4, 0)
    assert qemu.read(GPIO + 0x3C) & (1 << 10), "Push-pull high overrides pulldown"
    print("PASS output readback, release, open-drain and board/internal pulls", flush=True)


def software_i2c_reaches_real_devices(qemu):
    qemu.write(GPIO + 0x14, SDA | SCL)
    for pin in (47, 48):
        qemu.write(GPIO + 0x74 + pin * 4, 4)
    qemu.write(GPIO + 0x30, SDA | SCL)

    def line(mask, high):
        qemu.write(GPIO + (0x14 if high else 0x18), mask)

    def start():
        line(SCL, False)
        line(SDA, True)
        line(SCL, True)
        line(SDA, False)
        line(SCL, False)

    def stop():
        line(SCL, False)
        line(SDA, False)
        line(SCL, True)
        line(SDA, True)

    def send(byte):
        for shift in range(7, -1, -1):
            line(SDA, bool(byte & (1 << shift)))
            line(SCL, True)
            line(SCL, False)
        line(SDA, True)
        line(SCL, True)
        ack = not (qemu.read(GPIO + 0x40) & SDA)
        line(SCL, False)
        return ack

    def receive(last):
        line(SDA, True)
        byte = 0
        for _ in range(8):
            line(SCL, True)
            byte = (byte << 1) | bool(qemu.read(GPIO + 0x40) & SDA)
            line(SCL, False)
        line(SDA, last)
        line(SCL, True)
        line(SCL, False)
        line(SDA, True)
        return byte

    def probe(address):
        start()
        ack = send(address << 1)
        stop()
        return ack

    def read_registers(address, register, count):
        start()
        assert send(address << 1) and send(register), "Register address must ACK"
        start()
        assert send(address << 1 | 1), "Repeated START/read must ACK"
        result = bytes(receive(i == count - 1) for i in range(count))
        stop()
        return result

    assert all(probe(address) for address in (0x32, 0x68, 0x15)), "RTC, IMU and touch must be discoverable"
    assert not probe(0x50), "Absent NFC must NACK"
    assert read_registers(0x6E, 0, 2) == bytes((0x50, 0x20)), "M5PM1 product ID is 0x2050"
    assert read_registers(0x68, 0, 1) == b"\x24", "BMI270 chip ID"
    assert read_registers(0x15, 0xA7, 1) == b"\xB5", "CST820 chip ID"
    # A hardware-I2C write must affect software probing of the same touch chip.
    qemu.write_register(0x4F, 3, 8)
    assert not probe(0x15), "Driving IOE IO4 low must reset the actual touch device"
    qemu.write_register(0x4F, 3, 0)
    assert probe(0x15), "Input/high-impedance IOE must release touch reset"
    # MCU reset releases an in-progress software transaction and keeps devices usable.
    start()
    assert send(0x64)
    qemu.qmp("system_reset")
    assert qemu.read_register(0x15, 0xA7) == 0xB5
    assert qemu.read_register(0x68, 0) == 0x24
    print("PASS software probe, repeated START/data, shared chip/reset state and MCU restart", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=ROOT / ".deps/qemu/build/qemu-system-xtensa")
    args = parser.parse_args()
    with QemuTest(args.flash, args.qemu) as qemu:
        gpio_drive_release_and_pulls(qemu)
        software_i2c_reaches_real_devices(qemu)


if __name__ == "__main__":
    main()
