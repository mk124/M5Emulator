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
project="$(cd "$(dirname "$0")/.." && pwd)"
base="$project/.deps/macos-x86_64"
prefix="$base/prefix-macos11"
qemu="$project/.deps/qemu"
export MACOSX_DEPLOYMENT_TARGET=11.0
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$prefix/share/pkgconfig"
export PKG_CONFIG_PATH=''
export PKG_CONFIG="$project/tools/pkg-config-static.sh"
bash "$project/tools/build_intel_dependencies.sh"
python3 "$project/tools/prepare_qemu.py"
mkdir -p "$qemu/build-x86_64" "$project/out/macos-x86_64"

cd "$qemu/build-x86_64"
../configure --target-list=xtensa-softmmu --cpu=x86_64 --cross-prefix= \
    --cc='/usr/bin/clang -arch x86_64' --cxx='/usr/bin/clang++ -arch x86_64' \
    --objcc='/usr/bin/clang -arch x86_64' --host-cc=/usr/bin/clang \
    --extra-cflags="-mmacosx-version-min=11.0 -Werror=unguarded-availability-new -I$prefix/include" --extra-cxxflags="-mmacosx-version-min=11.0 -Werror=unguarded-availability-new -I$prefix/include" \
    --extra-ldflags="-L$prefix/lib -Wl,-rpath,$prefix/lib" \
    --enable-gcrypt --disable-gnutls --disable-slirp --disable-user --disable-capstone \
    --disable-vnc --disable-gtk --enable-sdl --disable-sdl-image --disable-cocoa \
    --disable-docs --disable-werror --disable-rust --disable-tools --disable-guest-agent \
    > "$project/out/macos-x86_64/qemu-configure.log" 2>&1
ninja -j "${M5EMU_BUILD_JOBS:-8}" qemu-system-xtensa > "$project/out/macos-x86_64/qemu-build.log" 2>&1

cmake -S "$project" -B "$project/build-x86_64" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OBJCXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
    -DM5EMU_QEMU_EXECUTABLE="$qemu/build-x86_64/qemu-system-xtensa"
cmake --build "$project/build-x86_64" --parallel "${M5EMU_BUILD_JOBS:-8}"
