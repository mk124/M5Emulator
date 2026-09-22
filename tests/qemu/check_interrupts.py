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

"""Verify pending and shared peripheral interrupt routing on real Xtensa CPUs."""

import argparse
from pathlib import Path
import re

from qemu_test import QemuTest, ROOT


def pending_interrupts_follow_routes(qemu):
    matrix = 0x600C2000
    spi = matrix + 21 * 4
    gpio = matrix + 16 * 4

    def pending(core=0):
        registers = qemu.qmp("human-monitor-command", {"command-line": "info registers -a"})
        return int(re.findall(r"\bINTERRUPT=([0-9a-fA-F]+)", registers)[core], 16)

    qemu.write(0x60024034, 0x1000)
    qemu.write(0x60024044, 0x1000)  # SPI is pending while disconnected.
    assert not pending() & (1 << 12)
    qemu.write(spi, 12)
    assert pending() & (1 << 12), "Connecting an already pending source must assert the CPU interrupt"
    qemu.write(spi, 13)
    assert pending() & (1 << 13) and not pending() & (1 << 12), "Remapping must release the old CPU input"
    qemu.write(spi + 0x800, 12)
    assert pending(1) & (1 << 12), "Both cores must see their own routes"

    qemu.write(gpio, 13)
    qemu.write(0x60004074, 1 << 13)
    qemu.write(0x60004048, 1)
    qemu.write(0x60024038, 0x1000)
    assert pending() & (1 << 13), "Clearing SPI must preserve the shared GPIO interrupt"
    assert not pending(1) & (1 << 12), "The second core has no GPIO sharing this input"
    qemu.write(0x6000404C, 1)
    assert not pending() & (1 << 13), "The final cleared source must release the shared interrupt"

    qemu.write(0x60024044, 0x1000)
    qemu.write(spi, 6)  # Internal timer interrupt: no peripheral route.
    assert not pending() & (1 << 13), "Disconnect must release an asserted peripheral IRQ"
    qemu.write(spi, 12)
    assert pending() & (1 << 12), "Reconnect must retain a still-pending level"
    qemu.qmp("system_reset")
    assert not pending() & ((1 << 12) | (1 << 13)), "Reset must disable old routes"
    print("PASS pending, remapped, shared, dual-core and reset interrupt routes", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=ROOT / ".deps/qemu/build/qemu-system-xtensa")
    args = parser.parse_args()
    with QemuTest(args.flash, args.qemu) as qemu:
        pending_interrupts_follow_routes(qemu)


if __name__ == "__main__":
    main()
