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

"""Apply correctness fixes and functional Light-sleep to the pinned QEMU checkout."""

from pathlib import Path
import subprocess

QEMU = Path(__file__).resolve().parents[1] / ".deps/qemu"


def main():
    fixes = {
        "include/hw/timer/esp_timg.h": [
            ("#define ESP_XTAL32K_FREQ        32000",
             "#define ESP_XTAL32K_FREQ        32768", 1),
        ],
        "hw/ssi/esp32s3_spi.c": [
            # Transfer bounds depend on the byte index, not the transmitted value.
            ("if (byte < tx_bytes)", "if (i < tx_bytes)", 1),
            ("if (byte < rx_bytes)", "if (i < rx_bytes)", 1),
        ],
        "hw/misc/ssi_psram.c": [
            # addr is signed; a negative address must never index host RAM.
            ("if (destination < size_bytes)",
             "if (destination >= 0 && destination < size_bytes)", 2),
        ],
        "target/xtensa/translate_tie_esp32s3.c": [
            # EE.VADDS/VSUBS saturate to the full signed range (S3 TRM 1.8).
            ("if (result < -0x7f) result = -0x7f;",
             "if (result < INT8_MIN) result = INT8_MIN;", 2),
            ("if (result < -0x7fff) result = -0x7fff;",
             "if (result < INT16_MIN) result = INT16_MIN;", 2),
            ("if (result < -0x7fffffff) result = -0x7fffffff;",
             "if (result < INT32_MIN) result = INT32_MIN;", 2),
        ],
    }

    changed = {}
    for relative, replacements in fixes.items():
        path = QEMU / relative
        original = text = path.read_text()
        for old, new, count in replacements:
            if text.count(old) == count:
                text = text.replace(old, new)
            elif text.count(new) != count:
                raise SystemExit(f"Unexpected QEMU source in {relative}: {old}")
        if text != original:
            changed[path] = text

    for path, text in changed.items():
        path.write_text(text)

    for name in (
        "spi-flash-write-enable.patch", "esp32s3-light-sleep.patch",
        "icount-deadlines.patch", "tcg-smp-timeslice.patch", "sdl-playback-drain.patch",
        "esp32s3-ble-hle.patch", "esp32s3-bt-clock.patch", "esp32s3-interrupt-routing.patch",
        "macos-deployment.patch"
    ):
        patch = Path(__file__).resolve().parents[1] / "patches" / name
        command = ["git", "apply", "--check", str(patch)]
        if subprocess.run(command, cwd=QEMU, capture_output=True).returncode == 0:
            subprocess.run(["git", "apply", str(patch)], cwd=QEMU, check=True)
        elif subprocess.run(command + ["--reverse"], cwd=QEMU, capture_output=True).returncode != 0:
            raise SystemExit(f"Unexpected QEMU source: {name} cannot be applied or verified")
    print(f"QEMU correctness fixes ready ({len(changed)} files updated)")


if __name__ == "__main__":
    main()
