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

"""Check screen power/reset through real QEMU I2C, GPIO and SPI controllers."""

import argparse
from pathlib import Path

from qemu_test import QemuTest, ROOT
from shared_state import BRIGHTNESS_OFFSET, HEADER, INPUT_OFFSET


def exercise(qemu):
    write, read, advance = qemu.write, qemu.read, qemu.advance
    read_register, write_register = qemu.read_register, qemu.write_register

    def expect(label, observed, expected):
        if observed != expected:
            raise AssertionError(f"{label}: got {observed:#x}, expected {expected:#x}")

    def probe_touch():
        return qemu.i2c((0x2A,), (0x3000, 0x0901, 0x1000))

    def outputs(value):
        # M5IOE1 IO4 = touch reset, IO5 = OLED reset, IO8 = OLED supply.
        write_register(0x4F, 0x05, value)

    def panel(dcs, data=()):
        # GPIO39 must drive the panel CS before transmitting SPI commands.
        write(0x60004030, 0x80)
        # SPI command/address phases carry the CO5300 four-byte command header.
        write(0x60004014, 0x80)
        write(0x60004018, 0x80)
        write(0x60024010, 0xC0000000 | (0x08000000 if data else 0))
        write(0x60024014, 23 << 27)
        write(0x60024018, 7 << 28 | 0x02)
        write(0x60024004, dcs << 16)
        write(0x6002401C, len(data) * 8 - 1 if data else 0)
        payload = bytes(data)
        for offset in range(0, len(payload), 4):
            write(0x60024098 + offset, int.from_bytes(payload[offset:offset + 4], "little"))
        write(0x60024000, 1 << 24)
        write(0x60004014, 0x80)

    def draw_red():
        panel(0x2A, (0, 6, 0, 6))
        panel(0x2B, (0, 0, 0, 0))
        panel(0x2C, (0xF8, 0))

    def shared_value(format, offset):
        advance(2000000)
        return qemu.read_shared(format, offset)[0]

    def pixel():
        return shared_value("=H", HEADER.size)

    def te_edge():
        write(0x6000410C, 0x2080)  # GPIO38 rising edge.
        write(0x60004058, 0x40)
        advance(40000000)
        return read(0x60004050) & 0x40

    def touch(down):
        qemu.write_shared("=iiI", INPUT_OFFSET + 4, 233, 233, int(down))
        advance(2000000)

    def touch_irq():
        return read(0x6000403C) & (1 << 13)

    write_register(0x4F, 0x03, 0x98)
    panel(0x11)
    panel(0x29)
    panel(0x51, (128,))
    draw_red()
    expect("Unpowered OLED is black", pixel(), 0)
    expect("Unpowered OLED rejects pixel writes", shared_value("=I", 60), 0)
    expect("Unpowered OLED has no TE", te_edge(), 0)
    expect("Touch held in reset NACKs", probe_touch(), 0x400)

    outputs(0x98)
    expect("Touch identity is readable while scanning initializes", read_register(0x15, 0xA7), 0xB5)
    write_register(0x15, 0xFA, 0x20)
    write_register(0x15, 0xED, 20)
    advance(100000000)
    expect("Power-on alone does not enable the display", pixel(), 0)
    panel(0x11)
    panel(0x29)
    panel(0x51, (128,))
    draw_red()
    expect("Brightness simulation defaults off", pixel(), 0xF800)
    before = shared_value("=Q", 64)
    qemu.write_shared("=I", BRIGHTNESS_OFFSET, 1)
    # RGB565 red at half brightness is 15/31 red, not the transmitted 31/31.
    expect("Enabling brightness simulation applies the stored brightness", pixel(), 0x7800)
    if shared_value("=Q", 64) == before:
        raise AssertionError("Toggling brightness did not publish an update for the frontend")
    qemu.write_shared("=I", BRIGHTNESS_OFFSET, 0)
    expect("Disabling brightness simulation restores unmodified GRAM", pixel(), 0xF800)
    panel(0x51, (0,))
    expect("Disabled brightness simulation ignores zero brightness", pixel(), 0xF800)
    panel(0x28)
    expect("Display Off still blanks with brightness simulation disabled", pixel(), 0)
    panel(0x29)
    panel(0x10)
    expect("Sleep still blanks with brightness simulation disabled", pixel(), 0)
    panel(0x11)
    expect("Waking restores pixels with brightness simulation disabled", pixel(), 0xF800)
    panel(0x51, (128,))
    qemu.write_shared("=I", BRIGHTNESS_OFFSET, 1)
    expect("Re-enabling brightness simulation preserves pixels and brightness", pixel(), 0x7800)
    print("PASS optional brightness applies live without pixel writes or reset; blanking is preserved", flush=True)
    panel(0x35, (0,))
    expect("Enabled TE reaches GPIO38", te_edge(), 0x40)
    print("PASS screen power gates SPI, visible pixels and TE", flush=True)

    outputs(0x88)
    expect("Hardware reset blanks the display", pixel(), 0)
    panel(0x11)
    panel(0x29)
    panel(0x51, (128,))
    panel(0x2A, (0, 6, 0, 6))
    panel(0x2B, (0, 0, 0, 0))
    panel(0x2C, (0, 0x1F))
    expect("Held reset keeps TE inactive", te_edge(), 0)
    outputs(0x98)
    panel(0x29)
    panel(0x51, (128,))
    advance(5000000)
    panel(0x11)
    panel(0x29)
    panel(0x51, (128,))
    expect("Reset during sleep-out requires recovery before waking", pixel(), 0)
    advance(120000000)
    expect("An early Sleep Out is ignored rather than queued", pixel(), 0)
    panel(0x11)
    expect("Hardware reset retains GRAM and rejects writes while held", pixel(), 0x7800)
    expect("Hardware reset disables TE until re-enabled", te_edge(), 0)
    panel(0x35, (0,))
    expect("TE restarts after reset", te_edge(), 0x40)
    print("PASS OLED reset retains GRAM, blocks commands and restores defaults", flush=True)

    panel(0x01)
    expect("Software reset blanks the display", pixel(), 0)
    expect("Software reset cancels TE", te_edge(), 0)
    advance(120000000)
    panel(0x11)
    panel(0x29)
    expect("Software reset restores zero brightness", pixel(), 0)
    panel(0x51, (128,))
    expect("Software reset preserves GRAM", pixel(), 0x7800)
    panel(0x01)
    advance(120000000)
    panel(0x11)
    panel(0x51, (128,))
    expect("Restoring brightness does not undo reset's Display Off", pixel(), 0)
    panel(0x29)
    expect("Display On reveals the retained GRAM", pixel(), 0x7800)
    panel(0x35, (0,))
    print("PASS software reset restores defaults without clearing GRAM", flush=True)

    qemu.qmp("system_reset")
    expect("MCU reset does not reset the external display", pixel(), 0x7800)
    expect("MCU reset leaves the external TE generator running", te_edge(), 0x40)
    print("PASS MCU reset preserves externally powered display state", flush=True)

    touch(True)
    outputs(0x18)
    panel(0x11)
    panel(0x29)
    panel(0x51, (128,))
    draw_red()
    expect("OLED power-off blanks the display", pixel(), 0)
    expect("OLED power-off rejects subsequent pixel writes", shared_value("=I", 60), 1)
    expect("OLED power-off cancels pending TE", te_edge(), 0)
    expect("OLED power-off leaves the L2 touch controller reachable", read_register(0x15, 0xA7), 0xB5)
    expect("OLED power-off preserves the held touch report", read_register(0x15, 0x02), 1)
    expect("Touch IRQ finishes its pulse while OLED is off", touch_irq(), 1 << 13)
    touch(False)
    expect("Touch release still produces an IRQ while OLED is off", touch_irq(), 0)
    outputs(0x98)
    advance(10000000)
    expect("Power restoration requires display initialization", pixel(), 0)
    panel(0x11)
    panel(0x29)
    panel(0x51, (128,))
    draw_red()
    expect("Display can be initialized after a power cycle", pixel(), 0x7800)
    print("PASS OLED power cycling preserves independent touch operation", flush=True)

    touch(True)
    expect("Touch pulse is active before reset", touch_irq(), 0)
    outputs(0x90)
    expect("Touch reset immediately releases IRQ", touch_irq(), 1 << 13)
    expect("Touch reset blocks I2C", probe_touch(), 0x400)
    touch(False)
    touch(True)
    advance(200000000)
    expect("A held reset never finishes initialization", probe_touch(), 0x400)
    expect("Input changes cannot assert IRQ during reset", touch_irq(), 1 << 13)
    outputs(0x98)
    expect("Touch reset release permits register access", read_register(0x15, 0xA7), 0xB5)
    write_register(0x15, 0xFA, 0x20)
    write_register(0x15, 0xED, 20)
    advance(99999999)
    expect("Touch reset recovery suppresses touch reports", read_register(0x15, 0x02), 0)
    expect("Touch reset recovery suppresses IRQ", touch_irq(), 1 << 13)
    advance(1)
    expect("A finger still present is reported after reset", read_register(0x15, 0x02), 1)
    expect("A finger still present is detected after reset", touch_irq(), 0)
    write_register(0x15, 0xE5, 3)
    expect("Sleep immediately releases touch IRQ", touch_irq(), 1 << 13)
    touch(False)
    touch(True)
    expect("Sleep suppresses new touch IRQs", touch_irq(), 1 << 13)
    outputs(0x90)
    advance(10000000)
    outputs(0x98)
    write_register(0x15, 0xFA, 0x20)
    advance(100000000)
    expect("Hardware reset exits touch sleep", touch_irq(), 0)
    print("PASS touch reset blocks I2C and reset/sleep suppress touch IRQs", flush=True)


def hardware_chip_select_reaches_panel(qemu):
    qemu.write_register(0x4F, 0x03, 0x98)
    qemu.write_register(0x4F, 0x05, 0x98)
    qemu.advance(150000000)  # Panel power/reset recovery before the first command.
    qemu.write(0x600045F0, 110)  # GPIO39 uses FSPICS0 and its peripheral output enable.

    def command(dcs, data=(), misc=0x3E):
        qemu.write(0x60024020, misc | ((1 << 30) if data else 0))
        qemu.write(0x60024010, 0xC0000000)
        qemu.write(0x60024014, 23 << 27)
        qemu.write(0x60024018, 7 << 28 | 0x02)
        qemu.write(0x60024004, dcs << 16)
        qemu.write(0x60024000, 1 << 24)
        if data:
            assert not qemu.read(0x60004040) & 0x80, "CS_KEEP_ACTIVE must keep GPIO39 low between header and data"
            qemu.write(0x60024020, misc)
            qemu.write(0x60024010, 1 << 27)
            qemu.write(0x6002401C, len(data) * 8 - 1)
            qemu.write(0x60024098, int.from_bytes(bytes(data), "little"))
            qemu.write(0x60024000, 1 << 24)
        assert qemu.read(0x60004040) & 0x80, "CS must return inactive after the transaction"

    def pixel():
        qemu.advance(2000000)
        return qemu.read_shared("=H", HEADER.size)[0]

    command(0x11)
    qemu.advance(150000000)
    command(0x29)
    command(0x2A, (0, 6, 0, 6))
    command(0x2B, (0, 0, 0, 0))
    command(0x2C, (0xF8, 0))
    assert pixel() == 0xF800, "Hardware CS and split transactions must deliver a red pixel to the real CO5300"
    command(0x28, misc=0x3F)
    assert pixel() == 0xF800, "Disabled CS0 must prevent the display-off command from reaching the panel"
    qemu.write(0x600045F0, 110 | (1 << 9))
    command(0x28, misc=0x3E | (1 << 7))
    assert pixel() == 0, "SPI CS polarity and matrix inversion must cancel and reach the panel"
    print("PASS hardware CS, retained selection, disable and polarity through the GPIO matrix", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=ROOT / ".deps/qemu/build/qemu-system-xtensa")
    args = parser.parse_args()

    with QemuTest(args.flash, args.qemu) as qemu:
        exercise(qemu)
    with QemuTest(args.flash, args.qemu) as qemu:
        hardware_chip_select_reaches_panel(qemu)


if __name__ == "__main__":
    main()
