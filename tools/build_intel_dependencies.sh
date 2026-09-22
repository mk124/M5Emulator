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
build="$base/build-macos11"
logs="$project/out/macos-x86_64"
jobs="${M5EMU_BUILD_JOBS:-8}"
manifest="$project/tools/intel-dependencies.txt"
mkdir -p "$base/downloads" "$base/src" "$logs"

# Only verified archives are extracted; interrupted downloads never become cache entries.
temporary=''
trap 'if [[ -n "$temporary" ]]; then rm -rf "$temporary"; fi' EXIT
while read -r name version checksum url; do
    [[ -z "$name" || "$name" == \#* ]] && continue
    archive="$base/downloads/${url##*/}"
    if [[ ! -f "$archive" ]]; then
        echo "Downloading $name $version"
        temporary="$(mktemp "$base/downloads/.${name}.XXXXXX")"
        curl --fail --location --retry 3 --connect-timeout 30 --output "$temporary" "$url"
        printf '%s  %s\n' "$checksum" "$temporary" | shasum -a 256 --check --status
        mv "$temporary" "$archive"
        temporary=''
    fi
    printf '%s  %s\n' "$checksum" "$archive" | shasum -a 256 --check --status
    source="$base/src/$name"
    if [[ -d "$source" ]]; then
        if [[ ! -f "$source/.archive-sha256" || "$(cat "$source/.archive-sha256")" != "$checksum" ]]; then
            echo "Source cache does not match $name $version: $source. Move it aside and retry." >&2
            exit 1
        fi
    else
        temporary="$(mktemp -d "$base/src/.${name}.XXXXXX")"
        tar -xf "$archive" -C "$temporary" --strip-components=1
        printf '%s\n' "$checksum" > "$temporary/.archive-sha256"
        mv "$temporary" "$source"
        temporary=''
    fi
done < "$manifest"

sdk="$(xcrun --show-sdk-path)"
recipe="$(cat "$0" "$manifest" "$project/tools/macos-x86_64.ini"; printf '%s\n' "$sdk"; xcrun clang --version)"
recipe="$(printf '%s' "$recipe" | shasum -a 256 | cut -d ' ' -f 1)"
if [[ ! -f "$build/.recipe" || "$(cat "$build/.recipe")" != "$recipe" ]]; then
    # Both directories contain generated files owned by this script.
    rm -rf "$build" "$prefix"
    mkdir -p "$build" "$prefix/lib/pkgconfig"
    printf '%s\n' "$recipe" > "$build/.recipe"
fi

export MACOSX_DEPLOYMENT_TARGET=11.0
export CC='/usr/bin/clang -arch x86_64'
export CXX='/usr/bin/clang++ -arch x86_64'
export OBJC='/usr/bin/clang -arch x86_64'
export OBJCXX='/usr/bin/clang++ -arch x86_64'
export CFLAGS="-O2 -mmacosx-version-min=11.0 -Werror=unguarded-availability-new -I$prefix/include"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-L$prefix/lib"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$prefix/share/pkgconfig"
export PKG_CONFIG_PATH=''
meson=(python3 "$base/src/meson/meson.py")
mkdir -p "$build" "$logs"
cmake_common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix")

# zlib is supplied by the selected Apple SDK. Its header provides the version.
zlib_version="$(sed -n 's/^#define ZLIB_VERSION "\(.*\)"/\1/p' "$sdk/usr/include/zlib.h")"
[[ -n "$zlib_version" ]] || { echo "Cannot determine SDK zlib version" >&2; exit 1; }
cat > "$prefix/lib/pkgconfig/zlib.pc" <<EOF
Name: zlib
Description: Apple SDK zlib
Version: $zlib_version
Libs: -lz
Cflags:
EOF

if [[ ! -f "$build/.sdl3-done" ]]; then
    echo 'Building SDL3 for x86_64'
    cmake -S "$base/src/sdl3" -B "$build/sdl3" "${cmake_common[@]}" -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF > "$logs/sdl3-build.log" 2>&1
    cmake --build "$build/sdl3" --parallel "$jobs" >> "$logs/sdl3-build.log" 2>&1
    cmake --install "$build/sdl3" >> "$logs/sdl3-build.log" 2>&1
    touch "$build/.sdl3-done"
fi
if [[ ! -f "$build/.sdl2-done" ]]; then
    echo 'Building SDL2 compatibility library for x86_64'
    cmake -S "$base/src/sdl2-compat" -B "$build/sdl2" "${cmake_common[@]}" -DSDL2COMPAT_TESTS=OFF > "$logs/sdl2-build.log" 2>&1
    cmake --build "$build/sdl2" --parallel "$jobs" >> "$logs/sdl2-build.log" 2>&1
    cmake --install "$build/sdl2" >> "$logs/sdl2-build.log" 2>&1
    touch "$build/.sdl2-done"
fi
if [[ ! -f "$build/.pcre2-done" ]]; then
    echo 'Building PCRE2 for x86_64'
    cmake -S "$base/src/pcre2" -B "$build/pcre2" "${cmake_common[@]}" -DBUILD_SHARED_LIBS=OFF -DPCRE2_BUILD_TESTS=OFF -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_PCRE2TEST=OFF > "$logs/pcre2-build.log" 2>&1
    cmake --build "$build/pcre2" --parallel "$jobs" >> "$logs/pcre2-build.log" 2>&1
    cmake --install "$build/pcre2" >> "$logs/pcre2-build.log" 2>&1
    touch "$build/.pcre2-done"
fi
if [[ ! -f "$build/.libgpg-error-done" ]]; then
    echo 'Building libgpg-error for x86_64'
    mkdir -p "$build/libgpg-error"
    (cd "$build/libgpg-error"
     "$base/src/libgpg-error/configure" --host=x86_64-apple-darwin --prefix="$prefix" --disable-shared --disable-nls --disable-doc --disable-tests > "$logs/libgpg-error-build.log" 2>&1
     make -j "$jobs" >> "$logs/libgpg-error-build.log" 2>&1
     make install >> "$logs/libgpg-error-build.log" 2>&1)
    touch "$build/.libgpg-error-done"
fi
if [[ ! -f "$build/.libgcrypt-done" ]]; then
    echo 'Building libgcrypt for x86_64'
    mkdir -p "$build/libgcrypt"
    (cd "$build/libgcrypt"
     "$base/src/libgcrypt/configure" --host=x86_64-apple-darwin --prefix="$prefix" --with-libgpg-error-prefix="$prefix" --disable-shared --disable-doc --disable-tests > "$logs/libgcrypt-build.log" 2>&1
     make -j "$jobs" >> "$logs/libgcrypt-build.log" 2>&1
     make install >> "$logs/libgcrypt-build.log" 2>&1)
    touch "$build/.libgcrypt-done"
fi
if [[ ! -f "$build/.pixman-done" ]]; then
    echo 'Building Pixman for x86_64'
    "${meson[@]}" setup --cross-file="$project/tools/macos-x86_64.ini" "$build/pixman" "$base/src/pixman" --prefix="$prefix" --libdir=lib --buildtype=release --default-library=static -Dtests=disabled -Ddemos=disabled > "$logs/pixman-build.log" 2>&1
    ninja -C "$build/pixman" -j "$jobs" >> "$logs/pixman-build.log" 2>&1
    ninja -C "$build/pixman" install >> "$logs/pixman-build.log" 2>&1
    touch "$build/.pixman-done"
fi
if [[ ! -f "$build/.gettext-done" ]]; then
    echo 'Building gettext runtime for x86_64'
    mkdir -p "$build/gettext"
    (cd "$build/gettext"
     "$base/src/gettext/gettext-runtime/configure" --host=x86_64-apple-darwin --prefix="$prefix" --disable-shared --disable-java --disable-libasprintf > "$logs/gettext-build.log" 2>&1
     make -C intl -j "$jobs" >> "$logs/gettext-build.log" 2>&1
     make -C intl install >> "$logs/gettext-build.log" 2>&1)
    touch "$build/.gettext-done"
fi
if [[ ! -f "$build/.libffi-done" ]]; then
    echo 'Building libffi for x86_64'
    mkdir -p "$build/libffi"
    (cd "$build/libffi"
     "$base/src/libffi/configure" --host=x86_64-apple-darwin --prefix="$prefix" --disable-shared --disable-docs > "$logs/libffi-build.log" 2>&1
     make -j "$jobs" >> "$logs/libffi-build.log" 2>&1
     make install >> "$logs/libffi-build.log" 2>&1)
    touch "$build/.libffi-done"
fi
if [[ ! -f "$build/.glib-done" ]]; then
    echo 'Building GLib for x86_64'
    "${meson[@]}" setup --cross-file="$project/tools/macos-x86_64.ini" "$build/glib" "$base/src/glib" --prefix="$prefix" --libdir=lib --buildtype=release --default-library=static --wrap-mode=nofallback -Dtests=false -Dinstalled_tests=false -Dintrospection=disabled -Ddocumentation=false -Dman-pages=disabled -Dnls=disabled > "$logs/glib-build.log" 2>&1
    ninja -C "$build/glib" -j "$jobs" >> "$logs/glib-build.log" 2>&1
    ninja -C "$build/glib" install >> "$logs/glib-build.log" 2>&1
    touch "$build/.glib-done"
fi
echo 'x86_64 dependencies built'
