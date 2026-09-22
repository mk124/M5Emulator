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

"""Check BMI270 interrupts through real qtest I2C MMIO, GPIOs and shared motion.

Checks both INT outputs and the board's RTC/Q7 combiner through
M5PM1 GPIO0 -> GPIO1 -> ESP32 GPIO12.
Register contracts: Bosch BST-BMI270-DS000-08 sections 4.6, 4.8.2, 4.9, 5.2.40.
"""

import argparse
import json
from pathlib import Path
import struct

from check_motion import expect, initialize_imu
from qemu_test import QemuTest, ROOT
from shared_state import MOTION_OFFSET


class ImuQemuTest(QemuTest):
    def __init__(self, *args):
        self.irq_levels = [0, 0]
        self.irq_edges = []
        super().__init__(*args)

    def command(self, text):
        # qtest forwards intercepted edges to the real board connection as well.
        self.stream.write((text + "\n").encode())
        while True:
            reply = self.stream.readline().decode().strip().split()
            if reply and reply[0] == "IRQ":
                pin, level = int(reply[2]), int(reply[1] == "raise")
                self.irq_levels[pin] = level
                self.irq_edges.append((pin, level))
            elif reply and reply[0] == "OK":
                return reply[1:]
            else:
                raise RuntimeError((text, reply))

    def intercept_imu(self):
        request = {"execute": "qom-list", "arguments": {"path": "/machine/unattached"}}
        self.monitor.write((json.dumps(request) + "\n").encode())
        while True:
            reply = json.loads(self.monitor.readline())
            if "error" in reply:
                raise RuntimeError(reply)
            if "return" in reply:
                device = next(item for item in reply["return"] if item["type"] == "child<bmi270>")
                self.command(f"irq_intercept_out /machine/unattached/{device['name']} irq")
                return


def configure_imu(qemu):
    qemu.write_register(0x68, 0x7E, 0xB6)
    initialize_imu(qemu)
    qemu.write_register(0x68, 0x40, 0x27)  # 50 Hz, normal/AVG4, power optimized.
    qemu.write_register(0x68, 0x41, 1)     # +/-4 g.
    qemu.write_register(0x68, 0x7D, 4)     # Accelerometer only.
    qemu.write_registers(0x68, 0x53, (0x0A, 0x08, 0))


def arm_motion(qemu, *, axes=7, output=None, latched=False):
    # Reset the reference while disabled, then use the same page RMW as Bosch's
    # base API. In particular, preserve the image's default out_conf selector.
    qemu.write_register(0x68, 0x7C, 2)
    qemu.write_register(0x68, 0x2F, 1)
    page = bytearray(qemu.read_registers(0x68, 0x30, 16))
    page[15] &= 0x7F
    qemu.write_registers(0x68, 0x30, page)
    qemu.write_shared("=6f", MOTION_OFFSET, 0, 0, 1, 0, 0, 0)
    qemu.advance(1000000)
    struct.pack_into("<H", page, 12, axes << 13 | 8)
    threshold_output = struct.unpack_from("<H", page, 14)[0]
    if output is not None:
        threshold_output = (threshold_output & ~0x7800) | output << 11
    struct.pack_into("<H", page, 14, (threshold_output & 0x7800) | 0x80FA)
    qemu.write_registers(0x68, 0x30, page)
    qemu.write_registers(0x68, 0x55, (int(latched), 0x40, 0, 0))
    qemu.write_register(0x68, 0x7C, 3)  # APS must not stop feature evaluation.
    qemu.read_registers(0x68, 0x1C, 2)
    qemu.irq_edges.clear()


def data_ready_pulses_without_reads(qemu):
    configure_imu(qemu)
    qemu.write_register(0x68, 0x58, 0x44)
    qemu.advance(156250)
    expect("Opposite INT polarities at rest", qemu.irq_levels, [0, 1])
    qemu.read_register(0x68, 0x1D)
    qemu.advance(19843749)
    expect("No early 50 Hz interrupt", qemu.irq_levels, [0, 1])
    qemu.advance(1)
    expect("Timer asserts both pins with no I2C reads", qemu.irq_levels, [1, 0])
    expect("Accelerometer interrupt source", qemu.read_register(0x68, 0x1D), 0x80)
    expect("Status read does not shorten a non-latched pulse", qemu.irq_levels, [1, 0])
    qemu.read_register(0x68, 0x0C)
    expect("A partial data read clears STATUS.drdy_acc", qemu.read_register(0x68, 0x03) & 0x80, 0)
    qemu.advance(156249)
    expect("DRDY pulse stays high for 156.25 us", qemu.irq_levels, [1, 0])
    qemu.advance(1)
    expect("DRDY pulse expires without a read", qemu.irq_levels, [0, 1])
    qemu.advance(20000000)
    expect("Unread DRDY status survives pulse expiry", qemu.read_register(0x68, 0x1D), 0x80)
    expect("DRDY status clears on read", qemu.read_register(0x68, 0x1D), 0)
    print("PASS autonomous DRDY, electrical polarity, pulse width and status lifetime", flush=True)


def latched_data_ready_obeys_pin_controls(qemu):
    configure_imu(qemu)
    qemu.write_registers(0x68, 0x55, (1, 0, 0, 4))
    qemu.read_register(0x68, 0x1D)
    qemu.advance(23000000)
    expect("Latched DRDY survives its pulse", qemu.irq_levels, [1, 1])
    qemu.read_registers(0x68, 0x0C, 6)
    expect("Data read does not acknowledge INT_STATUS_1", qemu.irq_levels, [1, 1])
    qemu.write_register(0x68, 0x58, 0)
    expect("Unmapping releases INT1", qemu.irq_levels, [0, 1])
    qemu.write_register(0x68, 0x54, 0x0A)
    qemu.write_register(0x68, 0x58, 0x40)
    expect("Pending DRDY maps independently to INT2", qemu.irq_levels, [0, 1])
    qemu.write_register(0x68, 0x54, 2)
    expect("Disabled output resolves low even with pending status", qemu.irq_levels, [0, 0])
    qemu.write_register(0x68, 0x54, 0x0A)
    expect("Re-enabled output drives pending IRQ", qemu.irq_levels, [0, 1])
    qemu.write_register(0x68, 0x54, 0x0C)  # Active-low open drain with pull-up.
    qemu.read_register(0x68, 0x1D)
    expect("Status acknowledgment releases open drain", qemu.irq_levels, [0, 1])
    qemu.write_register(0x68, 0x7D, 2)
    qemu.read_register(0x68, 0x1D)
    qemu.advance(10000000)
    expect("Gyroscope alone produces its own DRDY source", qemu.read_register(0x68, 0x1D), 0x40)
    qemu.write_register(0x68, 0x7D, 0)
    qemu.irq_edges.clear()
    qemu.advance(100000000)
    expect("Disabled sensors cancel DRDY timers", qemu.irq_edges, [])
    print("PASS latched DRDY, mapping, output enable, open drain and gyro-only sampling", flush=True)


def sensor_configuration_preserves_independent_cadence(qemu):
    qemu.write_register(0x68, 0x7E, 0xB6)
    initialize_imu(qemu)  # Both sensors start at 100 Hz.
    qemu.write_registers(0x68, 0x53, (0x0A, 0x08, 1, 0, 0, 4))
    qemu.advance(20000000)
    qemu.read_register(0x68, 0x1D)

    qemu.advance(3000000)
    qemu.write_register(0x68, 0x42, 0xA9)  # Gyro changes to 200 Hz at 23 ms.
    expect("A rate write cannot fabricate a completed sample", qemu.read_register(0x68, 0x1D), 0)
    expect("Reconfiguring gyro cannot assert accelerometer IRQ", qemu.irq_levels[0], 0)
    qemu.advance(1000000)
    qemu.write_register(0x68, 0x43, 2)     # Range writes must preserve both clocks.
    qemu.advance(1000000)
    qemu.write_register(0x68, 0x41, 1)
    qemu.advance(2999999)
    expect("Range writes cannot create or advance DRDY", qemu.read_register(0x68, 0x1D), 0)
    qemu.advance(1)
    expect("200 Hz gyro completes after its 5 ms period", qemu.read_register(0x68, 0x1D), 0x40)
    qemu.advance(2000000)
    expect("100 Hz accelerometer keeps its original cadence", qemu.read_register(0x68, 0x1D), 0x80)

    qemu.advance(1000000)
    qemu.write_register(0x68, 0x40, 0xA7)  # Acceleration changes to 50 Hz at 31 ms.
    expect("Accelerometer rate write produces no immediate DRDY", qemu.read_register(0x68, 0x1D), 0)
    qemu.advance(2000000)
    expect("Accelerometer reconfiguration preserves the gyro deadline", qemu.read_register(0x68, 0x1D), 0x40)
    print("PASS independent sensor cadence across rate and range changes", flush=True)


def motion_qualifies_threshold_duration_and_axes(qemu):
    configure_imu(qemu)
    arm_motion(qemu, axes=1)
    qemu.write_shared("=6f", MOTION_OFFSET, 0.1, 0.25, 1, 1000, 1000, 1000)
    qemu.advance(200000000)
    expect("Subthreshold X, unselected Y and gyro cannot trigger", qemu.read_register(0x68, 0x1C), 0)
    qemu.write_shared("=6f", MOTION_OFFSET, 0.25, 0, 1, 0, 0, 0)
    qemu.advance(140000000)
    expect("Seven qualifying samples are insufficient", qemu.irq_levels[0], 0)
    qemu.write_shared("=6f", MOTION_OFFSET, 0, 0, 1, 0, 0, 0)
    qemu.advance(20000000)
    qemu.write_shared("=6f", MOTION_OFFSET, 0.25, 0, 1, 0, 0, 0)
    qemu.advance(159999999)
    expect("A below-threshold sample restarts the duration", qemu.irq_levels[0], 0)
    qemu.advance(1)
    expect("Eight consecutive 20 ms samples assert INT1", qemu.irq_levels[0], 1)
    expect("ANY_MOT status bit", qemu.read_register(0x68, 0x1C), 0x40)
    expect("Non-latched feature survives status read", qemu.irq_levels[0], 1)
    qemu.advance(20000000)
    expect("Settled acceleration releases the feature pin", qemu.irq_levels[0], 0)
    qemu.write_shared("=6f", MOTION_OFFSET, 0.5, 0, 1, 0, 0, 0)
    qemu.advance(180000000)
    expect("A second motion uses the updated reference", qemu.read_register(0x68, 0x1C), 0x40)
    expect("Read-clear applies after the feature pulse", qemu.read_register(0x68, 0x1C), 0)
    print("PASS any-motion axes, threshold, consecutive duration, reference and non-latch", flush=True)


def feature_pages_and_enable_gate_motion(qemu):
    configure_imu(qemu)
    arm_motion(qemu)
    qemu.write_register(0x68, 0x2F, 2)
    qemu.write_registers(0x68, 0x3C, (0, 0, 0, 0))
    qemu.write_shared("=6f", MOTION_OFFSET, 0, 0.25, 1, 0, 0, 0)
    qemu.advance(160000000)
    expect("Writing another feature page cannot disable any-motion", qemu.read_register(0x68, 0x1C), 0x40)
    arm_motion(qemu, output=4, latched=True)
    qemu.write_registers(0x68, 0x56, (0x40, 0x08))
    qemu.write_shared("=6f", MOTION_OFFSET, 0, 0, 1.25, 0, 0, 0)
    qemu.advance(180000000)
    expect("Feature output selector and INT2 map are respected", qemu.irq_levels, [0, 0])
    expect("Selected feature status survives settled motion in latch mode", qemu.read_register(0x68, 0x1C), 0x08)
    expect("Feature acknowledgment releases active-low INT2", qemu.irq_levels, [0, 1])
    for gate in ("feature", "axes", "output", "accelerometer"):
        arm_motion(qemu, axes=0 if gate == "axes" else 7, output=0 if gate == "output" else 7)
        if gate == "feature":
            qemu.write_registers(0x68, 0x3E, (0xFA, 0x38))
        elif gate == "accelerometer":
            qemu.write_register(0x68, 0x7D, 0)
        qemu.write_shared("=6f", MOTION_OFFSET, 0.5, 0.5, 1.5, 0, 0, 0)
        qemu.advance(200000000)
        expect(f"Disabled {gate} suppresses any-motion", qemu.read_register(0x68, 0x1C), 0)
        expect(f"Disabled {gate} produces no IRQ", qemu.irq_edges, [])
    print("PASS feature-page isolation, output selector, latch and enable gates", flush=True)


def reset_cancels_interrupt_timers(qemu):
    configure_imu(qemu)
    arm_motion(qemu, latched=True)
    qemu.write_shared("=6f", MOTION_OFFSET, 0.25, 0, 1, 0, 0, 0)
    qemu.advance(140000000)
    qemu.write_register(0x68, 0x7E, 0xB6)
    qemu.irq_edges.clear()
    qemu.advance(100000000)
    expect("Reset cancels a partly qualified motion", qemu.irq_edges, [])
    expect("Reset clears all interrupt status", qemu.read_registers(0x68, 0x1C, 2), b"\x00\x00")
    configure_imu(qemu)
    arm_motion(qemu, latched=True)
    qemu.write_shared("=6f", MOTION_OFFSET, 0.25, 0, 1, 0, 0, 0)
    qemu.advance(160000000)
    expect("Motion is latched before reset", qemu.irq_levels[0], 1)
    qemu.qmp("system_reset")
    qemu.advance(20000000)
    expect("MCU reset preserves a latched external-chip IRQ", qemu.irq_levels[0], 1)
    expect("MCU reset preserves BMI configuration", qemu.read_register(0x68, 0x21), 1)
    expect("MCU reset preserves the pending source", qemu.read_register(0x68, 0x1C), 0x40)
    qemu.write_shared("=6f", MOTION_OFFSET, 0.5, 0, 1, 0, 0, 0)
    qemu.advance(160000000)
    expect("MCU reset preserves feature configuration", qemu.irq_levels[0], 1)
    qemu.write_register(0x68, 0x7E, 0xB6)
    expect("Soft reset releases both physical pins", qemu.irq_levels, [0, 0])
    qemu.irq_edges.clear()
    qemu.advance(100000000)
    expect("Expired pre-reset timers cannot reassert IRQ", qemu.irq_edges, [])
    print("PASS soft reset cancels IRQs; MCU reset preserves external BMI state", flush=True)


def pmic_latches_both_motion_edges(qemu):
    configure_imu(qemu)
    arm_motion(qemu)
    qemu.write_register(0x6E, 0x10, 2)
    qemu.write_register(0x6E, 0x16, 4)
    qemu.write_registers(0x6E, 0x43, (0x1E, 0xFF, 0xFF))
    qemu.write_registers(0x6E, 0x40, (0, 0, 0))
    qemu.write(0x600040A4, 0x2100)
    expect("PMIC IRQ starts released", qemu.read(0x6000403C) & (1 << 12), 1 << 12)
    qemu.write_shared("=6f", MOTION_OFFSET, 0.25, 0, 1, 0, 0, 0)
    qemu.advance(160000000)
    expect("BMI INT1 reaches Q7 without guest reads", qemu.irq_levels[0], 1)
    expect("PMIC asynchronously latches the assertion edge", qemu.read_register(0x6E, 0x40) & 1, 1)
    expect("PMIC drives GPIO12 low", qemu.read(0x6000403C) & (1 << 12), 0)
    qemu.read_register(0x68, 0x1C)
    qemu.write_register(0x6E, 0x40, 0xFE)
    expect("W0C releases PMIC IRQ during the BMI pulse", qemu.read(0x6000403C) & (1 << 12), 1 << 12)
    qemu.advance(20000000)
    expect("BMI pulse ends", qemu.irq_levels[0], 0)
    expect("PMIC also latches the deassertion edge", qemu.read_register(0x6E, 0x40) & 1, 1)
    qemu.advance(100000000)
    expect("GPIO12 stays low after BMI settles", qemu.read(0x6000403C) & (1 << 12), 0)
    qemu.write_register(0x6E, 0x40, 0xFE)
    expect("Clearing the second edge releases GPIO12", qemu.read(0x6000403C) & (1 << 12), 1 << 12)
    print("PASS BMI INT1 -> Q7 -> PMIC edge latches -> GPIO12, with no guest CPU", flush=True)


def rtc_and_imu_share_the_interrupt_line(qemu):
    configure_imu(qemu)
    arm_motion(qemu, latched=True)
    qemu.write_register(0x32, 0x1E, 0x40)
    qemu.write_registers(0x32, 0x10, (0x59, 0x34, 0x12, 0x02, 0x14, 0x09, 0x26))
    qemu.write_registers(0x32, 0x17, (0x80, 0x80, 0x80))
    qemu.write_register(0x32, 0x1D, 0)
    qemu.write_register(0x32, 0x1E, 0x08)
    qemu.write_shared("=6f", MOTION_OFFSET, 0.25, 0, 1, 0, 0, 0)
    qemu.advance(1002000000)
    expect("IMU is independently pending", qemu.irq_levels[0], 1)
    expect("RTC is independently pending", qemu.read_register(0x32, 0x1D) & 8, 8)
    qemu.read_register(0x68, 0x1C)
    expect("Clearing IMU cannot release the RTC's shared line", qemu.read_register(0x6E, 0x12) & 1, 0)
    qemu.write_register(0x6E, 0x40, 0)
    qemu.write_register(0x32, 0x1D, 0xF7)
    expect("Last source releases the shared line", qemu.read_register(0x6E, 0x12) & 1, 1)
    expect("Last release reaches PMIC's edge latch", qemu.read_register(0x6E, 0x40) & 1, 1)
    qemu.write_shared("=6f", MOTION_OFFSET, 0.5, 0, 1, 0, 0, 0)
    qemu.advance(60100000000)
    qemu.write_register(0x32, 0x1D, 0xF7)
    expect("Clearing RTC cannot release the IMU's shared line", qemu.read_register(0x6E, 0x12) & 1, 0)
    qemu.read_register(0x68, 0x1C)
    expect("Clearing the remaining IMU source releases the line", qemu.read_register(0x6E, 0x12) & 1, 1)
    print("PASS simultaneous RTC/IMU IRQs retain each other's pending level", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=ROOT / ".deps/qemu/build/qemu-system-xtensa")
    args = parser.parse_args()
    with ImuQemuTest(args.flash, args.qemu) as qemu:
        qemu.intercept_imu()
        data_ready_pulses_without_reads(qemu)
        latched_data_ready_obeys_pin_controls(qemu)
        sensor_configuration_preserves_independent_cadence(qemu)
        motion_qualifies_threshold_duration_and_axes(qemu)
        feature_pages_and_enable_gate_motion(qemu)
        reset_cancels_interrupt_timers(qemu)
        pmic_latches_both_motion_edges(qemu)
        rtc_and_imu_share_the_interrupt_line(qemu)


if __name__ == "__main__":
    main()
