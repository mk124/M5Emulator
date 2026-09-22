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

"""Check RTC alarms through the real I2C, PMIC and ESP32 GPIO devices.

Epson RX8130CE application manual ETM50E-10, sections 14.1 and 14.3:
https://download.epsondevice.com/td/pdf/app/RX8130CE_en.pdf
QemuTest selects -rtc clock=vm so alarms advance with the virtual clock.
"""

import argparse
from pathlib import Path

from qemu_test import QemuTest, ROOT

RTC = 0x32
PMIC = 0x6E
GPIO12 = 1 << 12


def expect(label, observed, expected):
    if observed != expected:
        raise AssertionError(f"{label}: got {observed!r}, expected {expected!r}")


def clear_chain(qemu):
    qemu.write_registers(PMIC, 0x40, (0, 0, 0))
    qemu.write(0x6000404C, GPIO12)


def expect_irq(qemu, pending):
    # Read the CPU-facing MMIO before any RTC access could synthesize an alarm.
    expect("ESP32 GPIO12 level", qemu.read(0x6000403C) & GPIO12, 0 if pending else GPIO12)
    expect("ESP32 GPIO12 falling-edge status", qemu.read(0x60004044) & GPIO12, GPIO12 if pending else 0)


def configure_alarm(qemu, calendar, alarm, *, day=False, enabled=True):
    qemu.write_register(RTC, 0x1E, 0x40)
    qemu.write_registers(RTC, 0x10, calendar)
    qemu.write_registers(RTC, 0x17, alarm)
    qemu.write_register(RTC, 0x1C, 0x08 if day else 0)
    qemu.write_register(RTC, 0x1D, 0)
    qemu.write_register(RTC, 0x1E, 0x08 if enabled else 0)
    clear_chain(qemu)


def initialize_chain(qemu):
    qemu.write_register(PMIC, 0x10, 0x02)
    qemu.write_register(PMIC, 0x16, 0x04)
    qemu.write_registers(PMIC, 0x43, (0x1E, 0xFF, 0xFF))
    clear_chain(qemu)
    qemu.write(0x600040A4, 0x2100)
    expect("RTC /INT starts released before any RTC access", qemu.read_register(PMIC, 0x12) & 1, 1)
    expect_irq(qemu, False)


def alarm_arrives_without_rtc_reads(qemu):
    configure_alarm(qemu, (0x58, 0x34, 0x12, 0x02, 0x14, 0x09, 0x26), (0x35, 0x12, 0x80))
    qemu.advance(1_999_999_999)
    expect_irq(qemu, False)
    # Allow the datasheet's maximum 1.46 ms alarm detection delay.
    qemu.advance(2_000_001)
    expect_irq(qemu, True)
    expect("AF latches at the programmed minute", qemu.read_register(RTC, 0x1D) & 0x08, 0x08)
    print("PASS timed RTC alarm reaches the CPU without I2C reads", flush=True)


def alarm_flags_and_enable_control_output(qemu):
    configure_alarm(qemu, (0x59, 0x00, 0x12, 0x02, 0x14, 0x09, 0x26), (0xFF, 0xFF, 0xFF), enabled=False)
    qemu.advance(1_002_000_000)
    expect_irq(qemu, False)
    expect("AIE=0 still records AF", qemu.read_register(RTC, 0x1D) & 0x08, 0x08)
    qemu.write_register(RTC, 0x1D, 0xFF)
    expect("Writing AF=1 preserves a pending alarm", qemu.read_register(RTC, 0x1D) & 0x08, 0x08)
    qemu.write_register(RTC, 0x1E, 0x08)
    expect_irq(qemu, True)

    qemu.write_register(RTC, 0x1E, 0)
    expect("Disabling AIE releases /INT", qemu.read_register(PMIC, 0x12) & 1, 1)
    expect("Disabling AIE retains AF", qemu.read_register(RTC, 0x1D) & 0x08, 0x08)
    clear_chain(qemu)
    expect_irq(qemu, False)
    qemu.write_register(RTC, 0x1E, 0x08)
    expect_irq(qemu, True)
    qemu.write_register(RTC, 0x1D, 0xF7)
    expect("Clearing AF releases /INT", qemu.read_register(PMIC, 0x12) & 1, 1)
    qemu.write_register(RTC, 0x1D, 0xFF)
    expect("Writing AF=1 cannot create an alarm", qemu.read_register(RTC, 0x1D) & 0x08, 0)
    clear_chain(qemu)
    qemu.advance(59_000_000_000)
    expect_irq(qemu, False)
    qemu.advance(1_000_000_000)
    expect_irq(qemu, True)
    print("PASS AF W0C, AIE gating and all-masked once-per-minute recurrence", flush=True)


def alarm_comparisons_respect_masks_and_wada(qemu):
    # 15 September 2026 is Tuesday; deliberately program the independent WEEK as Monday.
    cases = (
        ("minute/hour/date match", (0x35, 0x12, 0x15), True, True),
        ("minute mismatch", (0x36, 0x12, 0x15), True, False),
        ("hour mismatch", (0x35, 0x13, 0x15), True, False),
        ("date mismatch", (0x35, 0x12, 0x16), True, False),
        ("masked minute", (0xFF, 0x12, 0x15), True, True),
        ("masked hour", (0x35, 0xFF, 0x15), True, True),
        ("masked date", (0x35, 0x12, 0xFF), True, True),
        ("multiple weekdays include programmed Monday", (0x35, 0x12, 0x22), False, True),
        ("WADA=0 treats 0x15 as weekday bits", (0x35, 0x12, 0x15), False, False),
        ("no weekdays selected", (0x35, 0x12, 0), False, False),
        ("masked weekday", (0x35, 0x12, 0x80), False, True),
    )
    for label, alarm, day, matches in cases:
        configure_alarm(qemu, (0x59, 0x34, 0x12, 0x02, 0x15, 0x09, 0x26), alarm, day=day)
        qemu.advance(1_002_000_000)
        expect_irq(qemu, matches)
        expect(label, qemu.read_register(RTC, 0x1D) & 0x08, 0x08 if matches else 0)
    print("PASS minute/hour/day comparisons, AE masks and independent WEEK matching", flush=True)


def stop_and_seconds_writes_retime_alarm(qemu):
    configure_alarm(qemu, (0x59, 0x34, 0x12, 0x02, 0x14, 0x09, 0x26), (0x35, 0x12, 0x80))
    qemu.advance(250_000_000)
    qemu.write_register(RTC, 0x1E, 0x48)
    qemu.advance(2_000_000_000)
    expect_irq(qemu, False)
    expect("STOP prevents AF", qemu.read_register(RTC, 0x1D) & 0x08, 0)
    expect("STOP freezes the calendar", qemu.read_registers(RTC, 0x10, 7), bytes((0x59, 0x34, 0x12, 0x02, 0x14, 0x09, 0x26)))
    qemu.write_register(RTC, 0x1E, 0x08)
    qemu.advance(749_999_999)
    expect_irq(qemu, False)
    qemu.advance(2_000_001)
    expect_irq(qemu, True)
    qemu.write_register(RTC, 0x1E, 0x48)
    expect("STOP does not release a latched alarm", qemu.read_register(PMIC, 0x12) & 1, 0)
    qemu.write_register(RTC, 0x1D, 0xF7)
    expect("AF can be cleared during STOP", qemu.read_register(PMIC, 0x12) & 1, 1)
    clear_chain(qemu)
    qemu.advance(2_000_000_000)
    expect_irq(qemu, False)

    configure_alarm(qemu, (0x58, 0x34, 0x12, 0x02, 0x14, 0x09, 0x26), (0x35, 0x12, 0x80))
    qemu.advance(500_000_000)
    qemu.write_register(RTC, 0x10, 0x59)
    qemu.advance(999_999_999)
    expect_irq(qemu, False)
    qemu.advance(2_000_001)
    expect_irq(qemu, True)
    print("PASS STOP freeze/resume, retained AF and SEC divider reset", flush=True)


def changing_an_armed_alarm_cancels_its_old_deadline(qemu):
    configure_alarm(qemu, (0x58, 0x34, 0x12, 0x02, 0x14, 0x09, 0x26), (0x35, 0x12, 0x80))
    qemu.advance(1_000_000_000)
    qemu.write_register(RTC, 0x17, 0x36)
    qemu.advance(1_002_000_000)
    expect_irq(qemu, False)
    qemu.advance(60_000_000_000)
    expect_irq(qemu, True)
    print("PASS alarm reprogramming cancels the old deadline", flush=True)


def calendar_rollovers_preserve_written_week(qemu):
    cases = (
        ("leap day and independent Saturday-to-Sunday WEEK", (0x59, 0x59, 0x23, 0x40, 0x28, 0x02, 0x24),
         (0x00, 0x00, 0x01), False, (0x00, 0x00, 0x00, 0x01, 0x29, 0x02, 0x24)),
        ("non-leap February", (0x59, 0x59, 0x23, 0x20, 0x28, 0x02, 0x25),
         (0x00, 0x00, 0x01), True, (0x00, 0x00, 0x00, 0x40, 0x01, 0x03, 0x25)),
        ("year rollover", (0x59, 0x59, 0x23, 0x10, 0x31, 0x12, 0x26),
         (0x00, 0x00, 0x01), True, (0x00, 0x00, 0x00, 0x20, 0x01, 0x01, 0x27)),
    )
    for label, calendar, alarm, day, expected in cases:
        configure_alarm(qemu, calendar, alarm, day=day)
        qemu.advance(1_002_000_000)
        expect_irq(qemu, True)
        expect(label, qemu.read_registers(RTC, 0x10, 7), bytes(expected))
    print("PASS leap/month/year rollovers and independent one-hot WEEK carry", flush=True)


def calendar_burst_and_invalid_date_preserve_calendar(qemu):
    # A single running-calendar write spans SEC through Control0, as I2C permits.
    qemu.write_registers(RTC, 0x10, (
        0x59, 0x59, 0x23, 0x10, 0x31, 0x01, 0x30,
        0x00, 0x00, 0x01, 0, 0, 0x08, 0, 0x08,
    ))
    clear_chain(qemu)
    qemu.advance(1_002_000_000)
    expect_irq(qemu, True)
    expect("Control0 write preserves the newly written date", qemu.read_registers(RTC, 0x10, 7),
           bytes((0x00, 0x00, 0x00, 0x20, 0x01, 0x02, 0x30)))
    qemu.write_register(RTC, 0x14, 0)
    expect("Invalid date exposes VLF", qemu.read_register(RTC, 0x1D) & 0x02, 0x02)
    expect("Invalid date preserves the last valid calendar", qemu.read_registers(RTC, 0x14, 3), bytes((0x01, 0x02, 0x30)))
    print("PASS calendar/control burst writes and invalid-date rejection", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=ROOT / ".deps/qemu/build/qemu-system-xtensa")
    args = parser.parse_args()
    with QemuTest(args.flash, args.qemu) as qemu:
        initialize_chain(qemu)
        alarm_arrives_without_rtc_reads(qemu)
        alarm_flags_and_enable_control_output(qemu)
        alarm_comparisons_respect_masks_and_wada(qemu)
        stop_and_seconds_writes_retime_alarm(qemu)
        changing_an_armed_alarm_cancels_its_old_deadline(qemu)
        calendar_rollovers_preserve_written_week(qemu)
        calendar_burst_and_invalid_date_preserve_calendar(qemu)


if __name__ == "__main__":
    main()
