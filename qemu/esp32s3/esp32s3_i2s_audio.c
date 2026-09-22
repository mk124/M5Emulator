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

/* I2S frame timing and DMA PCM packing; serial edges are not simulated. */

#include "qemu/osdep.h"
#include "esp32s3_i2s.h"

#include "../audio/i2s_codec.h"

#include <math.h>

static unsigned dmaChannels(const Esp32s3I2S* i2s, unsigned direction)
{
	if (direction == ESP_GDMA_IN_IDX) return (i2s->registers[RxConf / 4] & Mono) ? 1 : 2;
	
	const uint32_t slots = i2s->registers[(RxTdm + 4) / 4];
	return (slots & (1 << 20)) ? 2 : ctpop32(slots & 3);
}

static unsigned sampleBytes(const Esp32s3I2S* i2s, unsigned direction)
{
	const uint32_t conf = i2s->registers[(RxConf + direction * 4) / 4];
	const unsigned bits = ((i2s->registers[(RxConf1 + direction * 4) / 4] >> 13) & 31) + 1;
	return bits == 24 && (conf & Fill24) ? 4 : bits / 8;
}

double esp32s3I2sBytePeriodNs(const Esp32s3I2S* i2s, unsigned direction)
{
	const uint32_t conf = i2s->registers[(RxConf + direction * 4) / 4];
	const uint32_t format = i2s->registers[(RxConf1 + direction * 4) / 4];
	const uint32_t slots = i2s->registers[(RxTdm + direction * 4) / 4];
	const unsigned bits = ((format >> 13) & 31) + 1;
	const unsigned bytes = sampleBytes(i2s, direction);
	const unsigned channels = dmaChannels(i2s, direction);
	
	if (!(conf & Start) || (conf & (Reset | FifoReset | Pdm)) ||
	    !(conf & Tdm) || !(conf & PcmBypass) ||
	    ((slots >> 16) & 15) != 1 || !channels || bits % 8 ||
	    !(i2s->registers[TxClk / 4] & ClockEnable) ||
	    !(i2s->registers[(RxClk + direction * 4) / 4] & ClockActive))
	{
		return 0;
	}
	
	unsigned clockDirection = direction;
	if (conf & Slave)
	{
		/* Only the internal full-duplex clock connection is modeled. */
		clockDirection = direction ^ 1;
		const uint32_t other = i2s->registers[(RxConf + clockDirection * 4) / 4];
		if (!(i2s->registers[TxConf / 4] & ShareClock) || !(other & Start) ||
		    (other & (Slave | Reset | FifoReset)))
		{
			return 0;
		}
	}
	
	static const unsigned SourceHz[] = { 40000000, 240000000, 160000000, 0 };
	const uint32_t clock = i2s->registers[(RxClk + clockDirection * 4) / 4];
	const unsigned source = (clock >> 27) & 3;
	if (!SourceHz[source] || !(clock & ClockActive)) return 0;
	
	/* Reverse the x/y/z/yn1 encoding used by i2s_ll_*_set_mclk(). */
	const uint32_t div = i2s->registers[(RxDiv + clockDirection * 4) / 4];
	const unsigned n = (clock & 255) ? (clock & 255) : 256;
	const unsigned z = div & 511, y = (div >> 9) & 511, x = (div >> 18) & 511;
	const unsigned a = z * (x + 1) + y;
	double fraction = a && z ? (double) z / a : 0;
	if ((div & (1 << 27)) && z)
	{
		fraction = 1 - fraction;
	}
	
	const uint32_t clockFormat = i2s->registers[(RxConf1 + clockDirection * 4) / 4];
	const double bitClockHz = SourceHz[source] / (n + fraction) /
	                          (((clockFormat >> 7) & 63) + 1);
	const double frameHz = bitClockHz / (2 * (((clockFormat >> 18) & 63) + 1));
	return 1e9 / (frameHz * bytes * channels);
}

void esp32s3I2sUpdateAudio(Esp32s3I2S* i2s, unsigned direction)
{
	if (!i2s->codec) return;
	
	const double periodNs = esp32s3I2sBytePeriodNs(i2s, direction);
	const unsigned frameBytes = sampleBytes(i2s, direction) * dmaChannels(i2s, direction);
	const unsigned rate = periodNs ? lround(1e9 / (periodNs * frameBytes)) : 0;
	I2S_CODEC_GET_CLASS(i2s->codec)->setRate(i2s->codec, direction == ESP_GDMA_IN_IDX, rate);
}

static int32_t loadSample(const uint8_t* data, unsigned bytes, bool bigEndian)
{
	uint32_t sample = 0;
	for (unsigned i = 0; i < bytes; i++)
	{
		sample |= (uint32_t) data[bigEndian ? bytes - 1 - i : i] << (i * 8);
	}
	return sample << (32 - bytes * 8);
}

static void storeSample(uint8_t* data, unsigned bytes, bool bigEndian, int32_t sample)
{
	const uint32_t value = (uint32_t) sample >> (32 - bytes * 8);
	for (unsigned i = 0; i < bytes; i++)
	{
		data[bigEndian ? bytes - 1 - i : i] = value >> (i * 8);
	}
}

void esp32s3I2sTransferAudio(Esp32s3I2S* i2s, unsigned direction, uint8_t* samples, unsigned bytes)
{
	if (!i2s->codec) return;
	
	const uint32_t conf = i2s->registers[(RxConf + direction * 4) / 4];
	const unsigned mask = i2s->registers[(RxTdm + direction * 4) / 4] & 3;
	const unsigned width = sampleBytes(i2s, direction), channels = dmaChannels(i2s, direction);
	const unsigned frameBytes = width * channels;
	if (!frameBytes || (conf & LsbFirst)) return; /* LSB-first serial mode is not modeled. */
	
	const bool input = direction == ESP_GDMA_IN_IDX;
	const bool bigEndian = conf & BigEndian;
	const unsigned polarity = (conf >> 17) & 1;
	I2sCodecClass* codec = I2S_CODEC_GET_CLASS(i2s->codec);
	
	while (bytes >= frameBytes)
	{
		int32_t frames[I2sBlockFrames][2] = { 0 };
		const unsigned count = MIN(bytes / frameBytes, I2sBlockFrames);
		if (input) codec->transfer(i2s->codec, true, frames, count);
		
		for (unsigned i = 0; i < count; i++)
		{
			if (input)
			{
				const unsigned monoChannel = mask == 2 ? 1 : mask == 1 ? 0 : !(conf & RxMonoFirst);
				for (unsigned channel = 0; channel < channels; channel++)
				{
					const unsigned slot = channels == 1 ? monoChannel : channel;
					const int32_t sample = (mask & (1 << slot)) ? frames[i][slot ^ polarity] : 0;
					storeSample(samples, width, bigEndian, sample);
					samples += width;
				}
			}
			else
			{
				for (unsigned slot = 0; slot < 2; slot++)
				{
					if (channels == 1 && !(mask & (1 << slot))) continue;
					const int32_t sample = loadSample(samples, width, bigEndian);
					samples += width;
					if (mask & (1 << slot)) frames[i][slot ^ polarity] = sample;
				}
				if (channels == 1 && (conf & TxRepeatMono))
				{
					const unsigned sourceSlot = mask == 2 ? 1 : 0;
					const unsigned targetSlot = mask == 2 ? 0 : 1;
					frames[i][targetSlot ^ polarity] = frames[i][sourceSlot ^ polarity];
				}
			}
		}
		
		if (!input) codec->transfer(i2s->codec, false, frames, count);
		bytes -= count * frameBytes;
	}
}
