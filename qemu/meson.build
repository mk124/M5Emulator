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

xtensa_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files(
  'audio/i2s_codec.c',
  'audio/recording_capture.c',
  'boards/m5_stopwatch.c',
  'esp32s3/esp32s3_gpio.c',
  'esp32s3/esp32s3_i2c.c',
  'esp32s3/esp32s3_i2s.c',
  'esp32s3/esp32s3_i2s_audio.c',
  'esp32s3/esp32s3_iomux.c',
  'esp32s3/esp32s3_spi.c',
  'esp32s3/esp32s3_sens.c',
  'devices/bmi270.c',
  'devices/co5300.c',
  'devices/cst820.c',
  'devices/es8311.c',
  'devices/es8311_audio.c',
  'devices/m5ioe1.c',
  'devices/m5pm1.c',
  'devices/rx8130ce.c',
))

if host_os == 'darwin'
  xtensa_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: declare_dependency(
    link_args: ['-Wl,-sectcreate,__TEXT,__info_plist,' + meson.current_source_dir() / 'audio/Info.plist'],
  ))
endif
