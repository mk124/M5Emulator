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
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

"""Build, test and bundle arm64/x86_64 as one relocatable macOS application."""

import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SYSTEM_PATHS = ("/usr/lib/", "/System/Library/")


def source_digest():
    paths = [ROOT / "CMakeLists.txt"]
    for directory in ("src", "shared", "qemu", "patches", "tools", "resources", "tests"):
        paths.extend(path for path in (ROOT / directory).rglob("*")
                     if path.is_file() and "__pycache__" not in path.parts and path.name != ".DS_Store")
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(str(path.relative_to(ROOT)).encode())
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.digest()


def run(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True).strip()


def macho(path):
    dependencies, rpaths, minimum, identity = [], [], "", ""
    command = ""
    for line in run("otool", "-l", path).splitlines():
        fields = line.split()
        if fields[:1] == ["cmd"]:
            command = fields[1]
        elif fields[:1] == ["name"]:
            name = line.strip()[5:].rsplit(" (offset", 1)[0]
            if command == "LC_ID_DYLIB":
                identity = name
            elif command in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB", "LC_LOAD_UPWARD_DYLIB"):
                dependencies.append(name)
        elif command == "LC_RPATH" and fields[:1] == ["path"]:
            rpaths.append(line.strip()[5:].rsplit(" (offset", 1)[0])
        elif fields[:1] == ["minos"] or (command == "LC_VERSION_MIN_MACOSX" and fields[:1] == ["version"]):
            minimum = fields[1]
    return dependencies, rpaths, minimum, identity


def version(value):
    return tuple(int(part) for part in value.split("."))


def copy_slice(source, destination, architecture):
    architectures = run("lipo", "-archs", source).split()
    if architecture not in architectures:
        raise RuntimeError(f"{source} has no {architecture} code")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if len(architectures) == 1:
        shutil.copy2(source, destination)
    else:
        run("lipo", source, "-thin", architecture, "-output", destination)
    destination.chmod(0o755)
    if subprocess.run(["codesign", "-d", str(destination)], capture_output=True).returncode == 0:
        run("codesign", "--remove-signature", destination)


def copy_licenses(source, destination):
    destination.mkdir(parents=True, exist_ok=True)
    for pattern in ("COPYING*", "LICENSE*", "LICENCE*", "LGPL*.txt", "NOTICE*"):
        for path in source.glob(pattern):
            if path.is_dir():
                shutil.copytree(path, destination / path.name, dirs_exist_ok=True)
            elif path.is_file():
                shutil.copy2(path, destination / path.name)


def bundle_architecture(contents, architecture, executable, qemu, sdl3):
    libraries = contents / "Frameworks" / architecture
    libraries.mkdir(parents=True)
    copied = {}
    minima = []
    inherited_rpaths = macho(executable)[1] + macho(qemu)[1]
    licenses = contents / "Resources/Licenses" / architecture

    def bundle(source, destination):
        if destination in copied:
            if copied[destination] != source.resolve():
                raise RuntimeError(f"Conflicting dependencies named {destination.name}")
            return

        copied[destination] = source.resolve()
        copy_slice(source, destination, architecture)
        dependencies, rpaths, minimum, identity = macho(destination)
        if not minimum:
            raise RuntimeError(f"Missing macOS deployment version: {source}")
        minima.append(minimum)

        for parent in source.resolve().parents:
            if (parent / "INSTALL_RECEIPT.json").exists():
                copy_licenses(parent, licenses / parent.parent.name)
                break

        for dependency in dependencies:
            if dependency.startswith(SYSTEM_PATHS):
                continue
            candidates = [dependency.replace("@loader_path", str(source.parent))]
            if dependency.startswith("@rpath/"):
                candidates = [str(Path(path.replace("@loader_path", str(source.parent))) / dependency[7:])
                              for path in rpaths + inherited_rpaths]
            resolved = next((Path(path) for path in candidates if Path(path).is_file()), None)
            if resolved is None:
                raise RuntimeError(f"Cannot resolve {dependency} used by {source}")
            target = libraries / resolved.name
            bundle(resolved, target)
            relative = os.path.relpath(target, destination.parent)
            run("install_name_tool", "-change", dependency, "@loader_path/" + relative, destination)

        if identity:
            run("install_name_tool", "-id", "@loader_path/" + destination.name, destination)
        for path in rpaths:
            run("install_name_tool", "-delete_rpath", path, destination)

    # SDL2-compat loads SDL3 with dlopen, so it is absent from otool -L.
    bundle(sdl3, libraries / "libSDL3.dylib")
    frontend = contents / "MacOS" / ("m5-emulator-" + architecture)
    bundle(executable, frontend)
    bundle(qemu, contents / "Helpers" / architecture / "qemu-system-xtensa")
    return frontend, max(minima, key=version)


def find_sdl3(architecture, override):
    if override is not None:
        path = override.expanduser().resolve()
        if not path.is_file() or architecture not in run("lipo", "-archs", path).split():
            raise RuntimeError(f"--sdl3-{architecture} must be a library containing {architecture} code: {path}")
        return path

    candidates = []
    if architecture == "x86_64":
        candidates.append(ROOT / ".deps/macos-x86_64/prefix-macos11/lib/libSDL3.dylib")
    # SDL2-compat can load SDL3 with dlopen; otool does not list that dependency.
    for module in ("sdl3", "sdl2"):
        result = subprocess.run(["pkg-config", "--variable=libdir", module], capture_output=True, text=True)
        if result.returncode == 0 and result.stdout.strip():
            candidates.append(Path(result.stdout.strip()) / "libSDL3.dylib")
    for path in candidates:
        if path.is_file() and architecture in run("lipo", "-archs", path).split():
            return path.resolve()
    raise RuntimeError(f"Cannot locate SDL3 for {architecture}; set --sdl3-{architecture} FILE")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "dist/M5 Emulator.app")
    parser.add_argument("--sdl3-arm64", type=Path, help="Override the arm64 SDL3 dynamic library")
    parser.add_argument("--sdl3-x86_64", type=Path, help="Override the x86_64 SDL3 dynamic library")
    args = parser.parse_args()
    for architecture, override in (("arm64", args.sdl3_arm64), ("x86_64", args.sdl3_x86_64)):
        if override is not None:
            find_sdl3(architecture, override)
    output = args.output.absolute()
    if output.suffix != ".app":
        parser.error("--output must name an .app bundle")
    if output.exists():
        with (output / "Contents/Info.plist").open("rb") as stream:
            if plistlib.load(stream).get("CFBundleIdentifier") != "dev.m5emulator":
                parser.error("refusing to replace an unrelated application")

    sources = source_digest()
    # Never combine a freshly built slice with an older cached executable.
    for script in ("build.sh", "build_intel.sh"):
        subprocess.run(["bash", str(ROOT / "tools" / script)], check=True, cwd=ROOT)
    for directory in ("build", "build-x86_64"):
        subprocess.run(["ctest", "--test-dir", str(ROOT / directory), "--output-on-failure"], check=True)

    output.parent.mkdir(parents=True, exist_ok=True)
    arm = ROOT / "build/m5-emulator.app"
    intel = ROOT / "build-x86_64/m5-emulator.app"
    with tempfile.TemporaryDirectory(prefix="m5emu-package-", dir=output.parent) as temporary:
        app = Path(temporary) / output.name
        shutil.copytree(arm, app)
        contents = app / "Contents"
        shutil.rmtree(contents / "_CodeSignature", ignore_errors=True)
        frontends, minima = [], {}
        inputs = {
            "arm64": (arm, ROOT / ".deps/qemu/build/qemu-system-xtensa", find_sdl3("arm64", args.sdl3_arm64)),
            "x86_64": (intel, ROOT / ".deps/qemu/build-x86_64/qemu-system-xtensa", find_sdl3("x86_64", args.sdl3_x86_64)),
        }
        for architecture, (build, qemu, sdl3) in inputs.items():
            # Resources must not depend on the CPU slice chosen by Launch Services.
            for resource in (build / "Contents/Resources").rglob("*"):
                if resource.is_file():
                    target = contents / "Resources" / resource.relative_to(build / "Contents/Resources")
                    if not target.exists() or target.read_bytes() != resource.read_bytes():
                        raise RuntimeError(f"Architecture resources differ: {resource}")
            frontend, minima[architecture] = bundle_architecture(
                contents, architecture, build / "Contents/MacOS/m5-emulator", qemu, sdl3)
            frontends.append(frontend)

        run("lipo", "-create", *frontends, "-output", contents / "MacOS/m5-emulator")
        for path in frontends:
            path.unlink()

        if version(minima["x86_64"]) > (11, 0):
            raise RuntimeError("Intel dependencies exceed the macOS 11 deployment target")
        with (contents / "Info.plist").open("rb") as stream:
            info = plistlib.load(stream)
        info["LSMinimumSystemVersion"] = min(minima.values(), key=version)
        info["LSMinimumSystemVersionByArchitecture"] = minima
        info["LSArchitecturePriority"] = ["arm64", "x86_64"]
        with (contents / "Info.plist").open("wb") as stream:
            plistlib.dump(info, stream)

        resources = contents / "Resources"
        (resources / "qemu").mkdir()
        shutil.copy2(ROOT / ".deps/qemu/pc-bios/esp32s3_rev0_rom.bin", resources / "qemu")
        copy_licenses(ROOT, resources / "Licenses/M5Emulator")
        copy_licenses(ROOT / ".deps/qemu", resources / "Licenses/QEMU")
        shutil.copy2(ROOT / ".deps/qemu/pc-bios/README", resources / "Licenses/QEMU/Firmware-README")
        for name in ("sdl2-compat", "sdl3", "glib", "pcre2", "pixman", "libgcrypt", "libgpg-error", "gettext", "libffi"):
            copy_licenses(ROOT / ".deps/macos-x86_64/src" / name, resources / "Licenses/x86_64" / name)
        copy_licenses(ROOT / ".deps/macos-x86_64/src/pcre2/deps/sljit", resources / "Licenses/x86_64/pcre2/sljit")
        copy_licenses(ROOT / ".deps/macos-x86_64/src/gettext/gettext-runtime/intl", resources / "Licenses/x86_64/gettext/intl")

        # Sign modified code from the inside out, then seal the application resources.
        for path in sorted((contents / "Frameworks").rglob("*.dylib")):
            run("codesign", "--force", "--sign", "-", path)
        for path in sorted((contents / "Helpers").rglob("qemu-system-xtensa")):
            run("codesign", "--force", "--sign", "-", path)
        run("codesign", "--force", "--sign", "-", app)
        run("codesign", "--verify", "--deep", "--strict", app)
        if source_digest() != sources:
            raise RuntimeError("Sources changed during the release build; rerun packaging")
        if output.exists():
            shutil.rmtree(output)
        app.rename(output)
    print(f"Packaged {output}")
    print("Minimum macOS: " + ", ".join(f"{arch} {value}" for arch, value in minima.items()))


if __name__ == "__main__":
    main()
