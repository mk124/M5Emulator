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

"""Apply the StopWatch overlay to a pinned Espressif QEMU checkout."""

from pathlib import Path
import shutil
import subprocess

import fix_qemu

ROOT = Path(__file__).resolve().parents[1]
QEMU = ROOT / ".deps/qemu"
REVISION = "febae182e132e4055529be423a818225ebddaa3a"


def patch_machine():
    path = QEMU / "hw/xtensa/esp32s3.c"
    original = path.read_text()
    # Rebuild the integration from the pinned source so cached hooks cannot go stale.
    text = subprocess.check_output(["git", "show", f"{REVISION}:hw/xtensa/esp32s3.c"], cwd=QEMU, text=True)
    text = text.replace('#include "cpu_esp32s3.h"', '#include "cpu_esp32s3.h"\n#include "m5stopwatch/boards/m5_stopwatch.h"')
    text = text.replace('    esp32s3_machine_init_sd(ss);', '''    if (object_dynamic_cast(OBJECT(machine), MACHINE_TYPE_NAME("m5stopwatch"))) {
        m5_stopwatch_init(OBJECT(ss), sys_mem, ESP_GDMA(&ss->gdma), DEVICE(&ss->rtc_cntl),
            qdev_get_gpio_in(intmatrix_dev, ETS_SPI2_INTR_SOURCE),
            qdev_get_gpio_in(intmatrix_dev, ETS_GPIO_INTR_SOURCE),
            qdev_get_gpio_in(intmatrix_dev, ETS_I2C_EXT0_INTR_SOURCE),
            qdev_get_gpio_in(intmatrix_dev, ETS_I2C_EXT1_INTR_SOURCE));
    }
    esp32s3_machine_init_sd(ss);''')
    text = text.replace('static void esp32s3_machine_type_init(void)', '''static void m5_stopwatch_class_init(ObjectClass *oc, void *data)
{
    MACHINE_CLASS(oc)->desc = "M5Stack StopWatch";
}

static const TypeInfo m5_stopwatch_info = {
    .name = MACHINE_TYPE_NAME("m5stopwatch"),
    .parent = TYPE_ESP32S3_MACHINE,
    .class_init = m5_stopwatch_class_init,
};

static void esp32s3_machine_type_init(void)''')
    text = text.replace('    type_register_static(&esp32s3_info);',
                        '    type_register_static(&esp32s3_info);\n    type_register_static(&m5_stopwatch_info);')

    text = text.replace('    if (s->requested_reset & ESP32S3_SOC_RESET_PERIPH) {', '''    if (s->requested_reset & ESP32S3_SOC_RESET_RTC) {
        device_cold_reset(DEVICE(&s->rtc_cntl));
    }
    if (s->requested_reset & ESP32S3_SOC_RESET_PERIPH) {''')
    # CPENABLE's architectural reset value is undefined. Choose an enabled
    # FPU for S3 startup; guest writes and disabled-coprocessor traps still apply.
    for core in range(2):
        reset = f"        cpu_reset(CPU(&s->cpu[{core}]));"
        text = text.replace(reset, reset + f"\n        s->cpu[{core}].env.sregs[CPENABLE] = 1;")

    text = text.replace('static void esp32s3_cpu_stall(void* opaque, int n, int level)\n{\n}', '''static void esp32s3_cpu_stall(void* opaque, int n, int level)
{
    Esp32s3SocState *s = opaque;
    if (s->cpu[n].env.runstall != (level != 0)) {
        if (!level) {
            cpu_reset_interrupt(CPU(&s->cpu[n]), CPU_INTERRUPT_HALT);
        }
        xtensa_runstall(&s->cpu[n].env, level != 0);
    }
}''')
    text = text.replace('    esp32s3_soc_add_periph_device(sys_mem, &s->rtc_cntl, DR_REG_RTCCNTL_BASE);', '''    esp32s3_soc_add_periph_device(sys_mem, &s->rtc_cntl, DR_REG_RTCCNTL_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->rtc_cntl), 0,
                       qdev_get_gpio_in(intmatrix_dev, ETS_RTC_CORE_INTR_SOURCE));''')

    text = text.replace('        sysbus_realize(SYS_BUS_DEVICE(&ss->systimer), &error_fatal);', '''        sysbus_realize(SYS_BUS_DEVICE(&ss->systimer), &error_fatal);
        qdev_connect_gpio_out_named(DEVICE(&ss->rtc_cntl), "sleep", 0,
                                    qdev_get_gpio_in_named(DEVICE(&ss->systimer), "sleep", 0));''')

    text = text.replace('#include "cpu_esp32s3.h"',
                        '#include "cpu_esp32s3.h"\n#include "m5stopwatch/esp32s3/esp32s3_sens.h"')
    text = text.replace('    esp32s3_machine_init_sd(ss);',
                        '    sysbus_create_simple(TYPE_ESP32S3_SENS, DR_REG_SENS_BASE, NULL);\n    esp32s3_machine_init_sd(ss);')

    # Descriptors contain full guest addresses, and display buffers live in
    # PSRAM. A DMA address space rooted at the DRAM bank cannot resolve either.
    text = text.replace('"soc_mr", OBJECT(dram), &error_abort);',
                        '"soc_mr", OBJECT(sys_mem), &error_abort);')
    if text != original:
        path.write_text(text)


def patch_build():
    path = QEMU / "hw/xtensa/meson.build"
    original = text = path.read_text()
    if "subdir('m5stopwatch')" not in text:
        text = text.replace("hw_arch +=", "subdir('m5stopwatch')\n\nhw_arch +=")
    if text != original:
        path.write_text(text)

    path = QEMU / "hw/xtensa/Kconfig"
    original = path.read_text()
    before, s3 = original.split("config XTENSA_ESP32S3", 1)
    if "select BITBANG_I2C" not in s3:
        path.write_text(before + "config XTENSA_ESP32S3" + s3.replace("    select SSI", "    select BITBANG_I2C\n    select SSI", 1))

    # This fork wraps a disabled dependency in declare_dependency(), making
    # slirp.found() true even when --disable-slirp was explicitly requested.
    path = QEMU / "meson.build"
    original = path.read_text()
    text = original.replace("if not get_option('slirp').auto() or have_system",
                            "if not get_option('slirp').disabled() and (get_option('slirp').enabled() or have_system)")
    if text != original:
        path.write_text(text)


def main():
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=QEMU, text=True).strip()
    if revision != REVISION:
        raise SystemExit(f"Expected QEMU {REVISION}, got {revision}")

    fix_qemu.main()
    overlay = QEMU / "hw/xtensa/m5stopwatch"
    if overlay.exists():
        shutil.rmtree(overlay)
    shutil.copytree(ROOT / "qemu", overlay)
    shutil.copy2(ROOT / "shared/include/stopwatch_shared.h", overlay / "stopwatch_shared.h")
    shutil.copy2(ROOT / "shared/include/recording_audio.h", overlay / "recording_audio.h")

    patch_machine()
    patch_build()
    print(f"Prepared StopWatch models in {QEMU}")


if __name__ == "__main__":
    main()
