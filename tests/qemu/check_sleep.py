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

"""Check Light-sleep timers and wake sources through the actual SoC registers."""

import argparse
from pathlib import Path

from qemu_test import QemuTest, ROOT
from shared_state import INPUT_OFFSET

RTC = 0x60008000
SYSTIMER = 0x60023000


def expect(label, observed, expected):
    if observed != expected:
        raise AssertionError(f"{label}: got {observed!r}, expected {expected!r}")


def rtc_ticks(qemu):
    qemu.write(RTC + 0x0C, 1 << 31)
    return qemu.read(RTC + 0x10) | qemu.read(RTC + 0x14) << 32


def system_ticks(qemu):
    qemu.write(SYSTIMER + 4, 1 << 30)
    return qemu.read(SYSTIMER + 0x44) | qemu.read(SYSTIMER + 0x40) << 32


def alarm(qemu, ticks):
    qemu.write(RTC + 4, ticks & 0xFFFFFFFF)
    qemu.write(RTC + 8, ticks >> 32)
    qemu.write(RTC + 0x4C, 1 << 10)
    qemu.write(RTC + 8, (ticks >> 32) | 1 << 16)


def sleep(qemu, sources):
    qemu.write(RTC + 0x4C, 3)
    qemu.write(RTC + 0x18, 2)
    qemu.write(RTC + 0x3C, sources << 15)
    qemu.write(RTC + 0x18, 1 << 31)


def sleeping(qemu):
    return bool(qemu.read(RTC + 0x18) & (1 << 31))


def timed_sleep_stops_only_the_system_clock(qemu):
    qemu.qmp("system_reset")
    alarm(qemu, rtc_ticks(qemu) + 1360)  # 10 ms with the calibrated RC clock.
    sleep(qemu, 8)
    before = system_ticks(qemu)
    qemu.advance(9000000)
    expect("Sleep remains active before the deadline", sleeping(qemu), True)
    expect("SYSTIMER stops during sleep", system_ticks(qemu), before)
    expect("RTC continues during sleep", rtc_ticks(qemu), 1224)
    qemu.advance(2000000)
    expect("RTC timer wakes without interrupt polling", sleeping(qemu), False)
    expect("Timer wake cause", qemu.read(RTC + 0x130), 8)
    expect("Wake and alarm status are latched", qemu.read(RTC + 0x44), 0x401)
    expect("Masked RTC interrupts do not assert", qemu.read(RTC + 0x48), 0)
    qemu.write(RTC + 0x138, 1)
    expect("W1TS exposes the pending wake", qemu.read(RTC + 0x48), 1)
    qemu.write(RTC + 0x13C, 1)
    expect("W1TC masks without acknowledging", qemu.read(RTC + 0x48), 0)
    qemu.write(RTC + 0x4C, 1)
    expect("Acknowledging wake preserves alarm status", qemu.read(RTC + 0x44), 0x400)
    before = system_ticks(qemu)
    qemu.advance(1000000)
    expect("SYSTIMER resumes at 16 MHz without counting sleep twice", system_ticks(qemu) - before, 16000)
    print("PASS timed wake, clock suspension/resume and RTC interrupt acknowledgement", flush=True)


def ext1_obeys_pin_selection_polarity_and_pending_levels(qemu):
    qemu.qmp("system_reset")
    qemu.write(RTC + 0xE0, (1 << 1) | (1 << 2))
    sleep(qemu, 8)  # EXT1 deliberately disabled.
    qemu.write_shared("=I", INPUT_OFFSET, 1)
    qemu.advance(2000000)
    expect("A pin cannot bypass the wake-source mask", sleeping(qemu), True)
    qemu.write(RTC + 0x3C, 2 << 15)
    expect("Enabling EXT1 observes an already-low selected pin", sleeping(qemu), False)
    expect("EXT1 records the pin that triggered", qemu.read(RTC + 0xE4), 1 << 2)
    expect("EXT1 wake cause", qemu.read(RTC + 0x130), 2)
    qemu.write_shared("=I", INPUT_OFFSET, 0)
    qemu.advance(2000000)
    expect("Releasing a pin does not clear wake status", qemu.read(RTC + 0xE4), 1 << 2)
    qemu.write(RTC + 0xE0, (1 << 22) | (1 << 1))
    expect("Explicit clear acknowledges EXT1 status", qemu.read(RTC + 0xE4), 0)
    sleep(qemu, 2)
    qemu.write_shared("=I", INPUT_OFFSET, 1)
    qemu.advance(2000000)
    expect("Unselected A cannot wake selected B", sleeping(qemu), True)
    qemu.write_shared("=I", INPUT_OFFSET, 3)
    qemu.advance(2000000)
    expect("Selected B wakes", sleeping(qemu), False)
    qemu.write(RTC + 0x64, 1 << 31)
    sleep(qemu, 2)
    expect("ANY_HIGH ignores held-low B", sleeping(qemu), True)
    qemu.write_shared("=I", INPUT_OFFSET, 0)
    qemu.advance(2000000)
    expect("ANY_HIGH wakes on release", sleeping(qemu), False)
    qemu.write(RTC + 0x64, 0)
    qemu.write(RTC + 0x68, (1 << 30) | (2 << 12))
    qemu.write_shared("=I", INPUT_OFFSET, 2)
    qemu.advance(2000000)
    sleep(qemu, 2)
    expect("Pending level rejects sleep immediately", sleeping(qemu), False)
    expect("Sleep reject is distinct from wake", qemu.read(RTC + 0x44), 2)
    expect("Reject cause is EXT1", qemu.read(RTC + 0x128), 2)
    qemu.write(RTC + 0x18, 2)
    expect("Reject cause has its own clear command", qemu.read(RTC + 0x128), 0)
    qemu.write_shared("=I", INPUT_OFFSET, 0)
    qemu.advance(2000000)
    print("PASS EXT1 selection, both polarities, latched status and sleep rejection", flush=True)


def rtc_clock_changes_agree_with_calibration(qemu):
    qemu.qmp("system_reset")
    # RTC and TIMG use different selector encodings. Compare the real counter
    # with the calibration result consumed by ESP-IDF, not a copied model constant.
    for rtc_source, calibration_source in ((0, 0), (1, 2), (2, 1)):
        before = rtc_ticks(qemu)
        qemu.write(RTC + 0x74, rtc_source << 30)
        expect("Changing the clock preserves accumulated ticks", rtc_ticks(qemu), before)
        qemu.write(0x6001F068, (1 << 31) | (10 << 16) | (calibration_source << 13))
        xtal_cycles = qemu.read(0x6001F06C) >> 7
        qemu.advance(1000000000)
        measured = rtc_ticks(qemu) - before
        measured_us = measured * xtal_cycles / 400
        if not 999500 <= measured_us <= 1000500:
            raise AssertionError(f"RTC source {rtc_source}: calibration reports {measured_us} us for 1 s")
    qemu.write(RTC + 0x74, 0)
    print("PASS continuous RTC clock switching and agreement with TIMG calibration", flush=True)


def alarm_reprogramming_and_reset_discard_old_deadlines(qemu):
    qemu.qmp("system_reset")
    alarm(qemu, 1360)
    alarm(qemu, (1 << 32) + 1360)
    sleep(qemu, 8)
    qemu.advance(20000000)
    expect("Alarm high word prevents the old low-word deadline", sleeping(qemu), True)
    alarm(qemu, rtc_ticks(qemu) + 1360)
    qemu.advance(5000000)
    expect("Reprogrammed deadline has not expired", sleeping(qemu), True)
    qemu.qmp("system_reset")
    qemu.advance(20000000)
    expect("Reset releases sleep", sleeping(qemu), False)
    expect("Reset cancels stale wake/alarm callbacks", qemu.read(RTC + 0x44), 0)
    qemu.write(RTC + 0x68, (1 << 30) | (8 << 12))
    alarm(qemu, rtc_ticks(qemu) - 1)
    sleep(qemu, 8)
    expect("Expired alarm rejects the next sleep", qemu.read(RTC + 0x128), 8)
    print("PASS 48-bit deadlines, reprogramming, reset cancellation and expired-alarm rejection", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=ROOT / ".deps/qemu/build/qemu-system-xtensa")
    args = parser.parse_args()
    with QemuTest(args.flash, args.qemu) as qemu:
        timed_sleep_stops_only_the_system_clock(qemu)
        ext1_obeys_pin_selection_polarity_and_pending_levels(qemu)
        rtc_clock_changes_agree_with_calibration(qemu)
        alarm_reprogramming_and_reset_discard_old_deadlines(qemu)


if __name__ == "__main__":
    main()
