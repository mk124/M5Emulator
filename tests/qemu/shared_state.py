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

"""Native host ABI shared with shared/include/stopwatch_shared.h, version 3."""

import struct

MAGIC = 0x4D355357
VERSION = 3
WIDTH = HEIGHT = 466
HEADER = struct.Struct("=IIIIIiiI6fHHIQQQ")
BRIGHTNESS_OFFSET = HEADER.size + WIDTH * HEIGHT * 2
SHARED_SIZE = BRIGHTNESS_OFFSET + 8  # uint32 option and native 8-byte struct alignment.
INPUT_OFFSET = 16
MOTION_OFFSET = 32
FEEDBACK_OFFSET = 56


def initialize(shared):
    HEADER.pack_into(shared, 0, MAGIC, VERSION, WIDTH, HEIGHT,
                     0, -1, -1, 0, 0, 0, 1, 0, 0, 0,
                     0, 0, 0, 0, 0, 0)
    struct.pack_into("=I", shared, BRIGHTNESS_OFFSET, 0)
