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

"""Control actual StopWatch QEMU devices through qtest, QMP and shared memory."""

from contextlib import ExitStack
import fcntl
import json
import mmap
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile

from shared_state import SHARED_SIZE, initialize

ROOT = Path(__file__).resolve().parents[2]


class QemuTest:
    def __init__(self, flash, qemu, *, audio="none,id=audio", microphone=False, env=None,
                 accel="qtest", serial="none", extra_args=()):
        with ExitStack() as resources:
            folder = Path(resources.enter_context(tempfile.TemporaryDirectory(prefix="m5emu-qtest-")))
            state = folder / "state.bin"
            self.shared_file = resources.enter_context(state.open("w+b"))
            self.shared_file.truncate(SHARED_SIZE)
            self.shared = resources.enter_context(mmap.mmap(self.shared_file.fileno(), 0))
            initialize(self.shared)
            shutil.copyfile(flash, folder / "flash.bin")

            test_host, test_guest = socket.socketpair()
            qmp_host, qmp_guest = socket.socketpair()
            for endpoint in (test_host, test_guest, qmp_host, qmp_guest):
                resources.enter_context(endpoint)
                endpoint.settimeout(5)
            process = subprocess.Popen([
                str(qemu), "-L", str(ROOT / ".deps/qemu/pc-bios"),
                "-machine", "m5stopwatch", "-m", "8M",
                "-global", "driver=ssi_psram,property=is_octal,value=true",
                "-drive", f"file={folder}/flash.bin,if=mtd,format=raw",
                "-rtc", "clock=vm",
                "-display", "none", "-monitor", "none", "-serial", serial,
                "-audiodev", audio, "-global", "es8311.audiodev=audio",
                "-global", f"es8311.microphone={'on' if microphone else 'off'}",
                "-chardev", f"socket,id=qt,fd={test_guest.fileno()}", "-qtest", "chardev:qt",
                "-chardev", f"socket,id=qm,fd={qmp_guest.fileno()}", "-qmp", "chardev:qm",
                "-qtest-log", "/dev/null", "-accel", accel, *extra_args,
            ], env=os.environ | (env or {}) | {"M5STOPWATCH_SHARED": str(state)},
                pass_fds=(test_guest.fileno(), qmp_guest.fileno()))
            resources.callback(self.stop, process)
            test_guest.close()
            qmp_guest.close()
            self.stream = resources.enter_context(test_host.makefile("rwb", buffering=0))
            self.monitor = resources.enter_context(qmp_host.makefile("rwb", buffering=0))
            greeting = json.loads(self.monitor.readline())
            if "QMP" not in greeting:
                raise RuntimeError(greeting)
            self.qmp("qmp_capabilities")
            self.resources = resources.pop_all()

    @staticmethod
    def stop(process):
        if process.poll() is not None:
            return
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

    def __enter__(self):
        return self

    def __exit__(self, *exception):
        self.resources.close()

    def qmp(self, name, arguments=None):
        request = {"execute": name}
        if arguments is not None:
            request["arguments"] = arguments
        self.monitor.write((json.dumps(request) + "\n").encode())
        while True:
            reply = json.loads(self.monitor.readline())
            if "return" in reply:
                return reply["return"]
            if "error" in reply:
                raise RuntimeError(reply)

    def command(self, text):
        self.stream.write((text + "\n").encode())
        reply = self.stream.readline().decode().strip()
        if not reply.startswith("OK"):
            raise RuntimeError((text, reply))
        return reply.split()[1:]

    def write(self, address, value):
        self.command(f"writel {address:#x} {value:#x}")

    def read(self, address):
        return int(self.command(f"readl {address:#x}")[0], 0)

    def advance(self, nanoseconds):
        self.command(f"clock_step {nanoseconds}")

    def read_shared(self, format, offset):
        fcntl.flock(self.shared_file, fcntl.LOCK_EX)
        try:
            return struct.unpack_from(format, self.shared, offset)
        finally:
            fcntl.flock(self.shared_file, fcntl.LOCK_UN)

    def write_shared(self, format, offset, *values):
        fcntl.flock(self.shared_file, fcntl.LOCK_EX)
        try:
            struct.pack_into(format, self.shared, offset, *values)
        finally:
            fcntl.flock(self.shared_file, fcntl.LOCK_UN)

    def i2c(self, data, commands):
        self.write(0x60013018, 0x3000)
        self.write(0x60013024, 0x3FFFF)
        for byte in data:
            self.write(0x6001301C, byte)
        for index, value in enumerate(commands):
            self.write(0x60013058 + index * 4, value)
        self.write(0x60013004, 0x30)
        return self.read(0x60013020) & 0x480

    def read_registers(self, device, register, count):
        if not 1 <= count <= 32:
            raise ValueError("I2C read must fit the controller's 32-byte FIFO")
        commands = [0x3000, 0x0902, 0x3000, 0x0901]
        if count > 1:
            commands.append(0x1800 | (count - 1))
        commands += [0x1C01, 0x1000]
        status = self.i2c((device << 1, register, device << 1 | 1), commands)
        if status != 0x80:
            raise AssertionError(f"I2C read at {device:#x}/{register:#x}: status {status:#x}")
        return bytes(self.read(0x6001301C) for _ in range(count))

    def read_register(self, device, register):
        return self.read_registers(device, register, 1)[0]

    def write_registers(self, device, register, values):
        data = bytes((device << 1, register)) + bytes(values)
        if not 3 <= len(data) <= 32:
            raise ValueError("I2C write must fit the controller's 32-byte FIFO")
        status = self.i2c(data, (0x3000, 0x0900 | len(data), 0x1000))
        if status != 0x80:
            raise AssertionError(f"I2C write at {device:#x}/{register:#x}: status {status:#x}")

    def write_register(self, device, register, value):
        self.write_registers(device, register, (value,))
