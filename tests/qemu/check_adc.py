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

"""Check RTC ADC conversions against QEMU's public SENS registers.

Prevents startup calibration hanging on DONE, writable conversion results,
cross-unit state leaks, and stale conversion state after reset.
"""

import argparse
from pathlib import Path

from qemu_test import QemuTest, ROOT


def check_conversions(qemu):
    adc1, adc2 = 0x6000880C, 0x60008830
    for control in (adc1, adc2):
        assert qemu.read(control) & 0x1FFFF == 0, "No conversion has completed at reset"
        qemu.write(control, 0x1FFFF)
        assert qemu.read(control) & 0x1FFFF == 0, "ADC result and DONE must be read-only"
        qemu.write(control, 0x20000)
        assert qemu.read(control) & 0x10000 == 0, "RTC software start must be selected"
        qemu.write(control, 0x40000)
        qemu.write(control, 0x60000)
        assert qemu.read(control) & 0x1FFFF == 0x10000, "Ground conversion must complete and return zero"
        if control == adc1:
            assert qemu.read(adc2) & 0x1FFFF == 0, "ADC1 must not complete ADC2"

    # Change ADC1's polarity between real conversions; ADC2 stays independent.
    qemu.write(0x60008800, qemu.read(0x60008800) | 0x10000000)
    qemu.write(adc1, 0x40000)
    qemu.write(adc1, 0x60000)
    assert qemu.read(adc1) & 0x1FFFF == 0x10FFF, "Inverted 12-bit ground sample must be full scale"
    assert qemu.read(adc2) & 0x1FFFF == 0x10000, "ADC1 polarity must not affect ADC2"
    qemu.write(adc1, 0x60000 | 0x1234)
    assert qemu.read(adc1) & 0x1FFFF == 0x10FFF, "Writing START high again must not overwrite the result"

    qemu.qmp("system_reset")
    for control in (adc1, adc2):
        assert qemu.read(control) & 0x1FFFF == 0, "Reset must clear prior conversions"
        qemu.write(control, 0x40000)
        qemu.write(control, 0x60000)
        assert qemu.read(control) & 0x1FFFF == 0x10000, "Conversion must work with reset polarity"
    print("PASS RTC ADC1/ADC2 conversion, read-only results, polarity and reset", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=ROOT / ".deps/qemu/build/qemu-system-xtensa")
    args = parser.parse_args()
    with QemuTest(args.flash, args.qemu) as qemu:
        check_conversions(qemu)


if __name__ == "__main__":
    main()
