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

"""Check six-axis sampling and motor/LED feedback through actual QEMU devices."""

import argparse
from pathlib import Path
import struct

from qemu_test import QemuTest, ROOT
from shared_state import FEEDBACK_OFFSET, MOTION_OFFSET


def expect(label, observed, expected):
    if observed != expected:
        raise AssertionError(f"{label}: got {observed!r}, expected {expected!r}")


def initialize_imu(qemu):
    # The existing model checks upload completeness; it does not execute microcode.
    qemu.write_register(0x68, 0x59, 0)
    qemu.write_registers(0x68, 0x5B, (0, 0))
    for offset in range(0, 8192, 30):
        qemu.write_registers(0x68, 0x5E, bytes(min(30, 8192 - offset)))
    qemu.write_register(0x68, 0x59, 1)
    expect("IMU initialization completes", qemu.read_register(0x68, 0x21), 1)
    qemu.write_register(0x68, 0x40, 0xA8)
    qemu.write_register(0x68, 0x42, 0xA8)
    qemu.write_register(0x68, 0x7D, 6)


def read_motion(qemu):
    return struct.unpack("<6h", qemu.read_registers(0x68, 0x0C, 12))


def physical_samples_respect_ranges(qemu):
    expect("Accelerometer reset range is 8 g", qemu.read_register(0x68, 0x41), 2)
    initialize_imu(qemu)
    qemu.write_shared("=6f", MOTION_OFFSET, 1, -1, 0.5, 125, -125, 62.5)

    # Signed 16-bit endpoint values follow the datasheet's full-scale ranges.
    accelerometer = ((16384, -16384, 8192), (8192, -8192, 4096),
                     (4096, -4096, 2048), (2048, -2048, 1024))
    for setting, expected in enumerate(accelerometer):
        qemu.write_register(0x68, 0x41, setting)
        qemu.advance(12000000)
        expect(f"Acceleration range {setting}", read_motion(qemu)[:3], expected)

    gyroscope = ((2048, -2048, 1024), (4096, -4096, 2048), (8192, -8192, 4096),
                 (16384, -16384, 8192), (32767, -32768, 16384))
    for setting, expected in enumerate(gyroscope):
        qemu.write_register(0x68, 0x43, setting)
        qemu.advance(12000000)
        expect(f"Gyroscope range {setting}", read_motion(qemu)[3:], expected)
    qemu.write_register(0x68, 0x43, 8)
    qemu.advance(12000000)
    expect("OIS range does not change primary gyro data", read_motion(qemu)[3:], (2048, -2048, 1024))

    qemu.write_shared("=6f", MOTION_OFFSET, 40, -40, 0, 4000, -4000, 0)
    qemu.advance(12000000)
    expect("Out-of-range input saturates without wrapping", read_motion(qemu), (32767, -32768, 0, 32767, -32768, 0))
    qemu.write_shared("=6f", MOTION_OFFSET, float("nan"), 0, 1, float("inf"), 0, 0)
    qemu.advance(12000000)
    expect("Non-finite input is rejected before integer conversion", read_motion(qemu), (0, 0, 2048, 0, 0, 0))
    print("PASS six-axis sign, range conversion and saturation", flush=True)


def sampling_respects_clock_and_power(qemu):
    qemu.write_register(0x68, 0x7D, 0)
    qemu.write_register(0x68, 0x41, 1)
    qemu.write_shared("=6f", MOTION_OFFSET, 1, 0, 0, 0, 0, 0)
    qemu.advance(1000000)
    qemu.write_register(0x68, 0x7D, 6)
    qemu.advance(20000000)
    expect("Initial 4 g sample", read_motion(qemu), (8192, 0, 0, 0, 0, 0))
    expect("Reading both sensors clears DRDY", qemu.read_register(0x68, 0x03) & 0xC0, 0)

    qemu.write_shared("=6f", MOTION_OFFSET, 0, -1, 0, 0, 0, 0)
    qemu.advance(2000000)
    expect("Input changes do not bypass ODR", read_motion(qemu), (8192, 0, 0, 0, 0, 0))
    qemu.advance(7999999)
    expect("No early 100 Hz DRDY", qemu.read_register(0x68, 0x03) & 0xC0, 0)
    qemu.advance(1)
    expect("Both sensors produce DRDY at 10 ms", qemu.read_register(0x68, 0x03) & 0xC0, 0xC0)
    expect("Next sample uses the changed acceleration", read_motion(qemu), (0, -8192, 0, 0, 0, 0))

    qemu.write_register(0x68, 0x7D, 0)
    qemu.write_shared("=6f", MOTION_OFFSET, 0, 0, -1, 0, 0, 0)
    qemu.advance(100000000)
    expect("Disabled sensors do not produce DRDY", qemu.read_register(0x68, 0x03) & 0xC0, 0)
    expect("Disabled sensors retain their last sample", read_motion(qemu), (0, -8192, 0, 0, 0, 0))
    qemu.write_register(0x68, 0x7D, 6)
    qemu.write_register(0x68, 0x40, 0xAD)
    qemu.write_register(0x68, 0x42, 0xA5)
    qemu.advance(100000000)
    expect("Reserved ODRs do not generate samples", qemu.read_register(0x68, 0x03) & 0xC0, 0)

    qemu.write_register(0x68, 0x7E, 0xB6)
    expect("Soft reset requires reinitialization", qemu.read_register(0x68, 0x21), 0)
    initialize_imu(qemu)
    qemu.advance(12000000)
    expect("Soft reset preserves physical orientation", read_motion(qemu), (0, 0, -4096, 0, 0, 0))
    print("PASS ODR, DRDY, sensor disable and soft-reset behavior", flush=True)


def motor_and_led_follow_firmware_control(qemu):
    def feedback():
        qemu.advance(2000000)
        return qemu.read_shared("=HH", FEEDBACK_OFFSET)

    expect("Startup motor off and status LED on", feedback(), (0, 1))
    qemu.write_register(0x4F, 0x04, 1)
    qemu.write_register(0x4F, 0x06, 1)
    expect("IO9 GPIO can drive the motor directly", feedback(), (65535, 1))
    qemu.write_register(0x4F, 0x04, 0)
    expect("Input mode releases the motor drive", feedback(), (0, 1))
    qemu.write_register(0x4F, 0x04, 1)
    qemu.write_register(0x4F, 0x06, 0)

    qemu.write_registers(0x4F, 0x1B, (0x55, 0x85))
    expect("PWM1 overrides GPIO with one-third duty", feedback(), (21845, 1))
    qemu.write_register(0x4F, 0x1C, 0xC5)
    expect("Inverted PWM drives two-thirds high", feedback(), (43690, 1))
    qemu.write_registers(0x4F, 0x25, (0, 0))
    expect("Stopped PWM frequency stops the drive", feedback(), (0, 1))
    qemu.write_registers(0x4F, 0x25, (0xF4, 1))
    expect("Restarted PWM restores its duty", feedback(), (43690, 1))
    qemu.write_register(0x4F, 0x04, 0)
    expect("PWM respects GPIO output mode", feedback(), (0, 1))
    qemu.write_register(0x4F, 0x04, 1)
    qemu.write_register(0x4F, 0x1C, 0x45)
    expect("Disabled PWM returns control to low GPIO", feedback(), (0, 1))
    qemu.write_register(0x4F, 0x06, 1)
    expect("GPIO regains motor control after PWM", feedback(), (65535, 1))
    qemu.write_register(0x4F, 0x29, 0x3A)
    expect("IOE reset stops the motor", feedback(), (0, 1))

    qemu.write_register(0x6E, 0x06, 0x07)
    expect("PMIC LED control clears the green indicator", feedback(), (0, 0))
    qemu.write_register(0x6E, 0x06, 0x17)
    expect("PMIC LED control restores the green indicator", feedback(), (0, 1))
    print("PASS GPIO/PWM motor drive, polarity, reset and PMIC LED control", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=ROOT / ".deps/qemu/build/qemu-system-xtensa")
    args = parser.parse_args()
    with QemuTest(args.flash, args.qemu) as qemu:
        physical_samples_respect_ranges(qemu)
        sampling_respects_clock_and_power(qemu)
        motor_and_led_follow_firmware_control(qemu)


if __name__ == "__main__":
    main()
