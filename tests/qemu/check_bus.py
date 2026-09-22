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

"""Check Flash WREN lifetime and both I2C masters through actual QEMU MMIO.

Flash contract: IS25LP128 datasheet, Write Enable and Page Program sections.
https://www.issi.com/WW/pdf/IS25LP128.pdf
Only QemuTest's temporary Flash copy is erased/programmed.
"""

import argparse
from pathlib import Path

from qemu_test import QemuTest, ROOT


def flash_operations_consume_write_enable(qemu):
    base, address = 0x60002000, 0xFF0000
    qemu.write(base + 0x34, 2)  # Select Flash only, not PSRAM.
    qemu.write(base + 0x1C, 23 << 26)

    def command(bit):
        qemu.write(base, 1 << bit)

    def status():
        command(27)
        return qemu.read(base + 0x2C) & 2

    def program(value):
        qemu.write(base + 4, address | 0x01000000)
        qemu.write(base + 0x58, value)
        command(25)

    def read_byte():
        qemu.write(base + 4, address)
        qemu.write(base + 0x28, 7)
        command(31)
        return qemu.read(base + 0x58) & 255

    command(30)
    assert status() == 2, "WREN must enable writing"
    qemu.write(base + 4, address)
    command(24)
    assert read_byte() == 255, "Sector erase must clear programmed data"
    assert status() == 0, "Sector erase must consume WREN"
    program(0)
    assert read_byte() == 255, "Programming without another WREN must be rejected"

    command(30)
    program(0xA5)
    assert read_byte() == 0xA5, "Enabled page program must store data"
    assert status() == 0, "Page program must consume WREN"
    program(0)
    assert read_byte() == 0xA5, "A second program requires another WREN"
    qemu.write(base + 4, address)
    command(24)
    assert read_byte() == 0xA5, "Erase without WREN must be rejected"
    print("PASS erase and page program consume WREN and protect subsequent writes", flush=True)


def both_i2c_masters_reach_the_same_devices(qemu):
    def transfer(base, data, commands, expected_status=0x80):
        qemu.write(base + 0x18, 0x3000)
        qemu.write(base + 0x24, 0x3FFFF)
        for byte in data:
            qemu.write(base + 0x1C, byte)
        for index, value in enumerate(commands):
            qemu.write(base + 0x58 + index * 4, value)
        qemu.write(base + 4, 0x30)
        assert qemu.read(base + 0x20) & 0x488 == expected_status, "I2C transaction status"

    def read_ioe(base, register):
        transfer(base, (0x9E, register, 0x9F), (0x3000, 0x0902, 0x3000, 0x0901, 0x1C01, 0x1000))
        return qemu.read(base + 0x1C)

    for base in (0x60013000, 0x60027000):
        assert read_ioe(base, 2) == ord("W"), "Both masters must find the fitted M5IOE1"

    # IOE IO4 releases the real CST820 from reset, observed through the other master.
    transfer(0x60027000, (0x9E, 3, 8), (0x3000, 0x0903, 0x1000))
    transfer(0x60027000, (0x9E, 5, 8), (0x3000, 0x0903, 0x1000))
    assert qemu.read_register(0x15, 0xA7) == 0xB5, "I2C1 IOE output must release the board touch device"
    # Reset the idle master while I2C0 retains its transaction across END.
    transfer(0x60013000, (0x9E, 2, 0x9F), (0x3000, 0x0902, 0x3000, 0x0901, 0x2000), 0x08)
    qemu.write(0x60027004, 1 << 10)
    transfer(0x60013000, (), (0x1C01, 0x1000))
    assert qemu.read(0x6001301C) == ord("W"), "Resetting idle I2C1 must not terminate I2C0 transaction"
    print("PASS both I2C masters reach the same board devices", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=ROOT / ".deps/qemu/build/qemu-system-xtensa")
    parser.add_argument("--only", choices=("flash", "i2c"))
    args = parser.parse_args()
    with QemuTest(args.flash, args.qemu) as qemu:
        if args.only != "i2c":
            flash_operations_consume_write_enable(qemu)
        if args.only != "flash":
            both_i2c_masters_reach_the_same_devices(qemu)


if __name__ == "__main__":
    main()
