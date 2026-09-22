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

"""Check speaker PCM and microphone DMA through actual QEMU audio backends."""

import argparse
import math
from pathlib import Path
import struct
from statistics import median
import tempfile
import time
import wave

from qemu_test import QemuTest, ROOT


def advance(qemu, milliseconds):
    for _ in range(milliseconds):
        qemu.advance(1000000)


def configure_codec(qemu):
    # IO3 supplies the codec and analog microphone; IO10 enables the amplifier.
    qemu.write_registers(0x4F, 0x03, (4, 2))
    qemu.write_registers(0x4F, 0x05, (4, 2))
    for register, value in ((0x00, 0x80), (0x01, 0x3F), (0x09, 0x0C), (0x0A, 0x0C),
                            (0x0D, 1), (0x0E, 2), (0x12, 0), (0x14, 0x1A),
                            (0x16, 0), (0x17, 0xBF), (0x32, 0xBF)):
        qemu.write_register(0x18, register, value)


def start_tx(qemu, *, bits=16, mono=False, padded=False, half_rate=False):
    # The two slots carry different frequencies, so an incorrect channel
    # selection cannot pass by playing an otherwise valid PCM buffer.
    payload = bytearray()
    width = 4 if padded else bits // 8
    shift = width * 8 - 16
    for frame in range(256):
        values = (round(10000 * math.sin(2 * math.pi * frame / 16)),)
        if not mono:
            values += (round(4000 * math.sin(2 * math.pi * frame / 8)),)
        for value in values:
            payload += (value << shift).to_bytes(width, "little", signed=True)
    qemu.command(f"write 0x3FC81000 {len(payload)} 0x{payload.hex()}")
    descriptor = struct.pack("<III", 0xC0000000 | len(payload) | len(payload) << 12, 0x3FC81000, 0x3FC80000)
    qemu.command(f"write 0x3FC80000 12 0x{descriptor.hex()}")
    qemu.write(0x6003F060, 4)
    qemu.write(0x6003F064, 0)  # Circular DMA without owner checking.
    qemu.write(0x6003F074, 0xFF)
    qemu.write(0x6003F0A8, 3)
    qemu.write(0x6003F080, 0x280000)

    # 240 MHz / 18.75 / 25 / 32 = 16 kHz. Wider slots double BCLK.
    slot_bits = 16 if bits == 16 else 32
    integer, fraction = (18, 0x203) if slot_bits == 16 else (9, 0xA03)
    if half_rate:
        integer, fraction = 37, 0x201
    qemu.write(0x6000F034, 0x2C000000 | integer)
    qemu.write(0x6000F03C, fraction)
    qemu.write(0x6000F02C, 24 << 7 | (bits - 1) << 13 | (slot_bits - 1) << 18 |
               (slot_bits - 1) << 24 | 1 << 29)
    qemu.write(0x6000F054, 0x10001 if mono else 0x10003)
    qemu.write(0x6000F024, 0x81004 | (0x60 if mono else 0) | (0x10000 if padded else 0))


def start_rx(qemu):
    descriptor = struct.pack("<III", 0x80000200, 0x3FC83000, 0x3FC82000)
    qemu.command(f"write 0x3FC82000 12 0x{descriptor.hex()}")
    qemu.write(0x6003F000, 0)
    qemu.write(0x6003F004, 0)
    qemu.write(0x6003F014, 0xFF)
    qemu.write(0x6003F048, 3)
    qemu.write(0x6003F020, 0x482000)
    qemu.write(0x6000F024, qemu.read(0x6000F024) | 1 << 27)
    qemu.write(0x6000F030, 1 << 26)
    qemu.write(0x6000F028, 24 << 7 | 15 << 13 | 15 << 18 | 15 << 24 | 1 << 29)
    qemu.write(0x6000F050, 0x10001)
    qemu.write(0x6000F064, 512)
    qemu.write(0x6000F020, 0x8122C)


def rms(samples):
    return math.sqrt(sum(value * value for value in samples) / len(samples))


def frequency(samples, sample_rate=16000):
    crossings = [i for i in range(1, len(samples)) if samples[i - 1] <= 0 < samples[i]]
    assert len(crossings) > 5, "PCM contains no sustained tone"
    return sample_rate / median(b - a for a, b in zip(crossings, crossings[1:]))


def read_wave(path):
    with wave.open(str(path)) as recording:
        assert recording.getparams()[:3] == (2, 2, 16000), recording.getparams()
        data = recording.readframes(recording.getnframes())
    samples = struct.unpack("<" + "h" * (len(data) // 2), data)
    assert samples[::2] == samples[1::2], "The single speaker must play equally on host left and right"
    return samples[::2]


def wav_backend(path):
    return f"wav,id=audio,path={str(path).replace(',', ',,')},out.frequency=16000,out.channels=2,out.format=s16"


def playback_respects_slots_format_clock_and_volume(flash, qemu_path, folder):
    cases = (("left", {}, 0x0C, 0xBF, 1000, 7071),
             ("right", {}, 0x8C, 0xBF, 2000, 2828),
             ("mono", {"mono": True}, 0x8C, 0xBF, 1000, 7071),
             ("packed24", {"bits": 24}, 0, 0xBF, 1000, 7071),
             ("padded24", {"bits": 24, "padded": True}, 0, 0xBF, 1000, 7071),
             ("pcm32", {"bits": 32}, 0x10, 0xBF, 1000, 7071),
             ("half_clock", {"half_rate": True}, 0x0C, 0xBF, 500, 7071),
             ("minus20db", {}, 0x0C, 0x97, 1000, 707))
    for name, config, serial, volume, expected_hz, expected_rms in cases:
        path = folder / f"{name}.wav"
        with QemuTest(flash, qemu_path, audio=wav_backend(path)) as qemu:
            qemu.qmp("cont")
            configure_codec(qemu)
            qemu.write_register(0x18, 9, serial)
            qemu.write_register(0x18, 0x32, volume)
            start_tx(qemu, **config)
            advance(qemu, 600)
            assert qemu.read(0x6003F068) & 7 == 3, "TX must complete without descriptor errors"
        samples = read_wave(path)
        assert 9000 < len(samples) <= 9600, (name, "wrong duration", len(samples))
        tail = samples[-3200:]
        assert abs(frequency(tail) - expected_hz) < 2, (name, "wrong pitch", frequency(tail))
        assert abs(rms(tail) - expected_rms) < expected_rms * 0.03, (name, "wrong gain", rms(tail))
    print("PASS speaker slot selection, mono duplication, PCM packing, clock and digital volume", flush=True)


def mute_and_amplifier_switch_live_audio(flash, qemu_path, folder):
    for name, device, register, disabled, enabled in (("sdp_mute", 0x18, 9, 0x4C, 0x0C),
                                                     ("dac_mute", 0x18, 0x31, 0x60, 0),
                                                     ("amplifier", 0x4F, 6, 0, 2)):
        path = folder / f"{name}.wav"
        with QemuTest(flash, qemu_path, audio=wav_backend(path)) as qemu:
            qemu.qmp("cont")
            configure_codec(qemu)
            start_tx(qemu)
            advance(qemu, 300)
            qemu.write_register(device, register, disabled)
            advance(qemu, 300)
            qemu.write_register(device, register, enabled)
            advance(qemu, 300)
        samples = read_wave(path)
        assert max(map(abs, samples[6400:8000])) == 0, (name, "muted audio is still audible")
        assert 6900 < rms(samples[-1600:]) < 7200, (name, "sound did not return")
    print("PASS live SDP/DAC mute and amplifier disable/re-enable", flush=True)


def amplifier_pulse_command_enables_real_audio(flash, qemu_path, folder):
    path = folder / "amplifier-pulse.wav"
    with QemuTest(flash, qemu_path, audio=wav_backend(path)) as qemu:
        qemu.qmp("cont")
        configure_codec(qemu)
        qemu.write_register(0x4F, 6, 0)
        qemu.write_register(0x4F, 4, 0)  # The command must select output mode itself.
        start_tx(qemu)
        qemu.write_register(0x4F, 0x90, 0x49)  # Two pulses on IO10, without REFRESH.
        advance(qemu, 300)
        qemu.write_register(0x4F, 0x90, 0xC9)
        advance(qemu, 300)
        assert qemu.read_register(0x4F, 8) & 2, "Pulse command left the amplifier disabled"
        assert qemu.read_register(0x4F, 0x90) == 0x49, "REFRESH did not self-clear"
        qemu.write_register(0x4F, 6, 0)  # Ordinary GPIO control must still disable it.
        qemu.write_register(0x4F, 0x90, 0xDF)  # Invalid pin must not wrap onto IO10.
        advance(qemu, 300)
        qemu.write_register(0x4F, 0x90, 0x89)  # No extra pulses: latch mode 1 high.
        advance(qemu, 300)
    samples = read_wave(path)
    assert max(map(abs, samples[1600:3200])) == 0, "Untriggered pulse configuration enabled audio"
    assert 6900 < rms(samples[6400:8000]) < 7200, "Pulse command failed to enable PCM output"
    assert max(map(abs, samples[11200:12800])) == 0, "GPIO disable or invalid pin handling failed"
    assert 6900 < rms(samples[-1600:]) < 7200, "Zero extra pulses failed to enable mode 1"
    print("PASS amplifier pulse trigger, pin selection, output mode, GPIO override and real PCM", flush=True)


def sdl_drains_short_clips_after_the_guest_stops(flash, qemu_path, folder):
    path = folder / "queued-speaker.raw"
    env = {"SDL_AUDIODRIVER": "disk", "SDL_DISKAUDIOFILE": str(path)}
    audio = "sdl,id=audio,out.frequency=44100,out.channels=2,out.format=s16,out.buffer-count=32"
    with QemuTest(flash, qemu_path, audio=audio, env=env) as qemu:
        qemu.qmp("cont")
        configure_codec(qemu)
        start_tx(qemu)
        # Produce 320 ms of PCM faster than the physical output consumes it.
        advance(qemu, 320)
        qemu.write(0x6000F024, qemu.read(0x6000F024) & ~4)
        advance(qemu, 20)
        qemu.qmp("stop")
        time.sleep(0.75)  # SDL must finish queued PCM even with virtual time stopped.

    stereo = struct.unpack("<" + "h" * (path.stat().st_size // 2), path.read_bytes())
    assert stereo[::2] == stereo[1::2], "Queued mono sound changed host channels"
    samples = stereo[::2]
    active = [i for i, value in enumerate(samples) if abs(value) > 10]
    assert active, "SDL did not play the queued clip"
    duration = (active[-1] - active[0] + 1) / 44100
    assert 0.318 <= duration <= 0.322, ("SDL truncated or split a 320 ms clip", duration)
    assert abs(frequency(samples[active[0]:active[-1] + 1], 44100) - 1000) < 3, "SDL changed the clip's pitch"
    print("PASS real SDL output drains a complete short clip after I2S and virtual time stop", flush=True)


def power_and_reset_preserve_dma_contract(flash, qemu_path):
    with QemuTest(flash, qemu_path) as qemu:
        assert qemu.i2c((0x30,), (0x3000, 0x0901, 0x1000)) == 0x400, "Unpowered codec must NACK"
        configure_codec(qemu)
        assert qemu.read_registers(0x18, 0xFD, 2) == b"\x83\x11"
        start_tx(qemu)
        qemu.advance(15999999)
        assert qemu.read(0x6003F068) & 2 == 0, "DMA completed before 256 frames elapsed"
        qemu.advance(1)
        assert qemu.read(0x6003F068) & 2, "No DMA completion without host audio"
        assert qemu.read(0x3FC80000) >> 31 == 0, "TX did not return descriptor ownership"

        qemu.write_register(0x18, 0, 0x1F)
        assert qemu.read_register(0x18, 0x32) == 0xBF, "Digital reset must retain the control bank"
        qemu.write_register(0x4F, 5, 0)
        assert qemu.i2c((0x30,), (0x3000, 0x0901, 0x1000)) == 0x400
        qemu.write(0x6003F074, 0xFF)
        advance(qemu, 20)
        assert qemu.read(0x6003F068) & 2, "Codec power loss must not stop MCU DMA"
        qemu.write_register(0x4F, 5, 4)
        assert qemu.read_register(0x18, 0x32) == 0, "Power-on must restore codec defaults"

        qemu.qmp("system_reset")
        advance(qemu, 20)
        assert qemu.read(0x6003F068) & 2 == 0, "MCU reset left an audio DMA timer running"
        start_tx(qemu)
        advance(qemu, 16)
        assert qemu.read(0x6003F068) & 2, "DMA could not restart after MCU reset"

        qemu.qmp("system_reset")
        second = struct.pack("<III", 0xC0400400, 0x3FC81000, 0)
        qemu.command(f"write 0x3FC8000C 12 0x{second.hex()}")
        start_tx(qemu)

        qemu.write(0x6000F024, 0)
        qemu.write(0x3FC80008, 0x3FC8000C)
        qemu.write(0x6000F024, 0x81004)
        # Both 16 ms descriptors are due even if the host wakes only at 32 ms.
        qemu.advance(32000000)
        assert qemu.read(0x3FC8000C) >> 31 == 0, "Late callbacks slowed the I2S sample clock"
        assert qemu.read(0x6003F068) & 8, "Delayed DMA did not reach the end of its list"
    print("PASS power NACK/POR, digital reset, DMA timing/ownership/catch-up and MCU reset/restart", flush=True)


def collect_microphone(qemu, milliseconds, realtime=True):
    result = []
    deadline = time.monotonic()
    for _ in range(milliseconds):
        qemu.advance(1000000)
        if realtime:
            deadline += 0.001
            time.sleep(max(0, deadline - time.monotonic()))
        if qemu.read(0x6003F008) & 2:
            data = bytes.fromhex(qemu.command("read 0x3FC83000 512")[0].removeprefix("0x"))
            result.extend(struct.unpack("<256h", data))
            qemu.write(0x6003F014, 0xFF)
    return result


def microphone_reaches_dma_with_gain_mute_and_opt_in(flash, qemu_path, folder):
    source = folder / "microphone.raw"
    source.write_bytes(b"".join(struct.pack("<hh", value, value)
                                 for i in range(441000)
                                 for value in (round(700 * math.sin(2 * math.pi * 1000 * i / 44100)),)))
    env = {"SDL_AUDIODRIVER": "disk", "SDL_DISKAUDIOFILEIN": str(source),
           "SDL_DISKAUDIOFILE": str(folder / "speaker.raw")}
    audio = "sdl,id=audio,in.frequency=44100,in.channels=2,in.format=s16,out.frequency=44100,out.channels=2"
    with QemuTest(flash, qemu_path, audio=audio, microphone=True, env=env) as qemu:
        qemu.qmp("cont")
        configure_codec(qemu)
        qemu.write_register(0x18, 0x44, 0x58)  # Espressif driver: ADC on left, DAC feedback on right.
        start_tx(qemu)
        start_rx(qemu)
        samples = collect_microphone(qemu, 500)[-3200:]
        peak = max(map(abs, samples))
        assert 680 < peak < 710, ("wrong ADC level", peak)
        assert abs(frequency(samples) - 1000) < 5, ("wrong ADC sample rate", frequency(samples))
        qemu.write_register(0x18, 0x16, 1)  # ADC_SCALE +6 dB.
        louder = collect_microphone(qemu, 300)[-1600:]
        assert 1.9 < max(map(abs, louder)) / peak < 2.1, "Microphone gain did not reach DMA"
        qemu.write_register(0x18, 0x0A, 0x4C)
        assert max(map(abs, collect_microphone(qemu, 100)[-512:])) == 0, "ADC mute retained old samples"
        qemu.write_register(0x18, 0x0A, 0x0C)
        assert max(map(abs, collect_microphone(qemu, 300)[-1600:])) > 1300, "Capture did not resume after unmute"
        qemu.write_register(0x4F, 5, 0)
        assert max(map(abs, collect_microphone(qemu, 100)[-512:])) == 0, "Unpowered microphone still supplies PCM"

    with QemuTest(flash, qemu_path, audio=audio, env=env) as qemu:
        qemu.qmp("cont")
        configure_codec(qemu)
        start_tx(qemu)
        start_rx(qemu)
        samples = collect_microphone(qemu, 100, realtime=False)
        assert samples and max(map(abs, samples)) == 0, "Microphone must be silent without opt-in"
    print("PASS real SDL capture -> ES8311 -> RX DMA, gain, mute, power and microphone opt-in", flush=True)


def main():
    parser = argparse.ArgumentParser(description="Check actual QEMU audio devices through MMIO, I2C and PCM output")
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=ROOT / ".deps/qemu/build/qemu-system-xtensa")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="m5emu-audio-test-") as directory:
        folder = Path(directory)
        playback_respects_slots_format_clock_and_volume(args.flash, args.qemu, folder)
        mute_and_amplifier_switch_live_audio(args.flash, args.qemu, folder)
        amplifier_pulse_command_enables_real_audio(args.flash, args.qemu, folder)
        sdl_drains_short_clips_after_the_guest_stops(args.flash, args.qemu, folder)
        power_and_reset_preserve_dma_contract(args.flash, args.qemu)
        microphone_reaches_dma_with_gain_mute_and_opt_in(args.flash, args.qemu, folder)


if __name__ == "__main__":
    main()
