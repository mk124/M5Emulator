#!/bin/bash
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

set -euo pipefail

project_dir="$(cd "$(dirname "$0")/.." && pwd)"
qemu_dir="$project_dir/.deps/qemu"
revision=febae182e132e4055529be423a818225ebddaa3a

if [[ ! -d "$qemu_dir/.git" ]]; then
    mkdir -p "$project_dir/.deps"
    git clone --depth 1 --branch esp-develop https://github.com/espressif/qemu.git "$qemu_dir"
    git -C "$qemu_dir" fetch --depth 1 origin "$revision"
    git -C "$qemu_dir" checkout --detach "$revision"
fi

python3 "$project_dir/tools/prepare_qemu.py"

if [[ ! -f "$qemu_dir/build/build.ninja" ]] || ! grep -Eq '^#define CONFIG_AUDIO_SDL($| )' "$qemu_dir/build/config-host.h"; then
    (
        cd "$qemu_dir"
        ./configure --target-list=xtensa-softmmu --enable-gcrypt \
            --disable-gnutls --disable-slirp --disable-user --disable-capstone \
            --disable-vnc --disable-gtk --enable-sdl --disable-sdl-image --disable-cocoa \
            --disable-docs --disable-werror --disable-rust --disable-tools \
            --disable-guest-agent
    )
fi
ninja -C "$qemu_dir/build" -j "${M5EMU_BUILD_JOBS:-8}" qemu-system-xtensa

cmake -S "$project_dir" -B "$project_dir/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$project_dir/build" --parallel "${M5EMU_BUILD_JOBS:-8}"
