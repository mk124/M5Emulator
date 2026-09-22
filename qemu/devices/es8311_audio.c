/**
 * Copyright (C) 2026 MK124 and contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* Functional PCM path: digital gain/mute and power, without analog DSP. */

#include "qemu/osdep.h"
#include "es8311.h"

#include "qemu/bswap.h"

#include "../audio/i2s_codec.h"

#include <math.h>

static void playbackAvailable(void* opaque, int available)
{
	Es8311* device = opaque;
	while (available >= sizeof(int32_t) && !fifo8_is_empty(&device->playback))
	{
		uint32_t bytes;
		const uint8_t* samples = fifo8_peek_bufptr(&device->playback, MIN(available, fifo8_num_used(&device->playback)), &bytes);
		const size_t written = AUD_write(device->output, (void*) samples, bytes);
		if (!written) break;
		fifo8_drop(&device->playback, written);
		available -= written;
	}
}

static void captureAvailable(void* opaque, int available)
{
	Es8311* device = opaque;
	int32_t stereo[I2sBlockFrames][2], samples[I2sBlockFrames];
	while (available >= sizeof(stereo[0]))
	{
		const size_t bytes = AUD_read(device->input, stereo, MIN(available, sizeof(stereo)));
		if (!bytes) break;
		
		const unsigned count = bytes / sizeof(stereo[0]);
		for (unsigned i = 0; i < count; i++)
		{
			/* QEMU's mono mixer sums channels; average explicitly to keep
			 * the microphone level independent of host channel count. */
			samples[i] = ((int64_t) stereo[i][0] + stereo[i][1]) / 2;
		}
		
		/* Keep recent input if the guest stops consuming it. */
		const unsigned monoBytes = count * sizeof(int32_t);
		const unsigned freeBytes = fifo8_num_free(&device->capture);
		if (monoBytes > freeBytes) fifo8_drop(&device->capture, monoBytes - freeBytes);
		fifo8_push_all(&device->capture, (const uint8_t*) samples, monoBytes);
		available -= bytes;
	}
}

void es8311UpdateAudio(Es8311* device)
{
	const uint8_t* registers = device->registers;
	
	/* CSM on, slave mode, digital/clock reset released. RST_MST is
	 * deliberately ignored in slave mode. */
	const bool digital = device->powered && (registers[Es8311ResetReg] & 0xD8) == 0x80;
	const bool analog = !(registers[Es8311SystemPowerReg] & 0xC0) && (registers[Es8311SystemPowerReg] & 3);
	const bool dac = digital && analog &&
	                 !(registers[Es8311ResetReg] & 1) && (registers[Es8311ClockReg] & 0x35) == 0x35 &&
	                 !(registers[Es8311SystemPowerReg] & 8) && !(registers[Es8311DacPowerReg] & 2) &&
	                 !(registers[Es8311RoutingReg] & 0x80);
	const bool adc = digital && analog &&
	                 !(registers[Es8311ResetReg] & 2) && (registers[Es8311ClockReg] & 0x3A) == 0x3A &&
	                 !(registers[Es8311SystemPowerReg] & 0x30) && !(registers[Es8311AnalogPowerReg] & 0x70) &&
	                 (registers[Es8311AnalogInputReg] & 0x70) == 0x10;
	
	device->outputGain = device->amplifier && !(registers[Es8311DacFormatReg] & 0x40) && !(registers[Es8311DacMuteReg] & 0x60)
	                         ? pow(10, (registers[Es8311DacVolumeReg] - 191) / 40.0)
	                         : 0;
	/* Host capture represents the ADC input; analog PGA/filter effects are
	 * omitted. The driver's microphone gain uses ADC_SCALE (6 dB/step). */
	device->inputGain = pow(10, ((registers[Es8311AdcVolumeReg] - 191) * 0.5 + (registers[Es8311AdcScaleReg] & 7) * 6) / 20.0);
	if (registers[Es8311AdcScaleReg] & 0x10) device->inputGain = -device->inputGain;
	
	device->outputActive = dac && device->outputRate >= 8000 && device->outputRate <= 96000;
	device->inputActive = adc && device->microphone && device->inputRate >= 8000 && device->inputRate <= 96000 &&
	                      !(registers[Es8311AdcFormatReg] & 0x40) && !(registers[Es8311AdcClockReg] & 0x10);
	if (device->outputActive)
	{
		struct audsettings settings = { device->outputRate, 1, AUDIO_FORMAT_S32, HOST_BIG_ENDIAN };
		device->output = AUD_open_out(&device->card, device->output, "es8311-speaker", device, playbackAvailable, &settings);
		device->outputActive = device->output != NULL;
	}
	if (device->inputActive)
	{
		struct audsettings settings = { device->inputRate, 2, AUDIO_FORMAT_S32, HOST_BIG_ENDIAN };
		device->input = AUD_open_in(&device->card, device->input, "es8311-microphone", device, captureAvailable, &settings);
		device->inputActive = device->input != NULL;
	}
	
	AUD_set_active_out(device->output, device->outputActive);
	AUD_set_active_in(device->input, device->inputActive);
	if (!device->outputActive || !device->outputGain) fifo8_reset(&device->playback);
	if (!device->inputActive)
	{
		fifo8_reset(&device->capture);
		device->capturePrimed = false;
	}
}

void es8311SetRate(Object* codec, bool input, unsigned rate)
{
	Es8311* device = ES8311(codec);
	unsigned* current = input ? &device->inputRate : &device->outputRate;
	if (*current == rate) return;
	
	*current = rate;
	fifo8_reset(input ? &device->capture : &device->playback);
	if (input) device->capturePrimed = false;
	es8311UpdateAudio(device);
}

static int32_t scaledSample(int32_t sample, double gain, uint8_t format)
{
	static const unsigned WordBits[] = { 24, 20, 18, 16, 32, 0, 0, 0 };
	const unsigned bits = WordBits[(format >> 2) & 7];
	if (!bits || (format & 3) > 1) return 0;
	
	const int32_t scaled = CLAMP(sample * gain, (double) INT32_MIN, (double) INT32_MAX);
	return (uint32_t) scaled & (UINT32_MAX << (32 - bits));
}

void es8311Transfer(Object* codec, bool input, int32_t frames[][2], unsigned count)
{
	Es8311* device = ES8311(codec);
	const uint8_t* registers = device->registers;
	int32_t samples[I2sBlockFrames] = { 0 };
	assert(count <= I2sBlockFrames);
	
	if (input)
	{
		const unsigned bytes = count * sizeof(int32_t);
		const unsigned available = fifo8_num_used(&device->capture);
		/* Host capture arrives in bursts. Prime 20 ms after startup or an
		 * underrun instead of splicing partial blocks into repeated silence. */
		if (!device->capturePrimed && available >= MAX(bytes, device->inputRate / 50 * sizeof(int32_t)))
		{
			device->capturePrimed = true;
		}
		if (available < bytes) device->capturePrimed = false;
		if (device->inputActive && device->capturePrimed) fifo8_pop_buf(&device->capture, (uint8_t*) samples, bytes);
		
		const unsigned route = (registers[Es8311RoutingReg] >> 4) & 7;
		const unsigned polarity = (registers[Es8311AdcFormatReg] >> 5) & 1;
		for (unsigned i = 0; i < count; i++)
		{
			const int32_t sample = scaledSample(samples[i], device->inputGain, registers[Es8311AdcFormatReg]);
			/* Preserve ADC slots in the driver's ADC + DAC feedback modes.
			 * Digital playback feedback itself is not synthesized. */
			frames[i][polarity] = route == 0 || route == 1 || route == 5 ? sample : 0;
			frames[i][polarity ^ 1] = route == 0 || route == 2 || route == 4 ? sample : 0;
		}
		return;
	}
	if (!device->outputActive) return;
	
	const unsigned channel = ((registers[Es8311DacFormatReg] >> 7) ^ (registers[Es8311DacFormatReg] >> 5)) & 1;
	for (unsigned i = 0; i < count; i++)
	{
		samples[i] = scaledSample(frames[i][channel], device->outputGain, registers[Es8311DacFormatReg]);
	}
	
	/* A stalled host must not stall virtual DMA or accumulate old sound. */
	const unsigned bytes = count * sizeof(int32_t);
	const unsigned freeBytes = fifo8_num_free(&device->playback);
	if (bytes > freeBytes) fifo8_drop(&device->playback, bytes - freeBytes);
	fifo8_push_all(&device->playback, (const uint8_t*) samples, bytes);
	playbackAvailable(device, fifo8_num_used(&device->playback));
}
