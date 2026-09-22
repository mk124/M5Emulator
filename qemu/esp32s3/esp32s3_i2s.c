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

/*
 * ESP32-S3 I2S0 model.
 *
 * Register contract: ESP-IDF 5.5.4 soc/esp32s3/register/soc/i2s_reg.h and
 * hal/esp32s3/include/hal/i2s_ll.h. RX/TX_UPDATE transfers configuration
 * between clock domains, then self-clears; it is not a DMA completion.
 * Standard PCM uses timed GDMA descriptor transfers connected to a codec.
 * Timing follows the programmed clock/slot format, on
 * QEMU's virtual clock, at descriptor boundaries rather than individual bits.
 * No external slave/MCLK input, PDM, >2-slot TDM, FIFO timing or
 * I2S-local interrupt output. DMA interrupts use the existing GDMA IRQs.
 */

#include "qemu/osdep.h"
#include "esp32s3_i2s.h"

#include "hw/dma/esp_gdma.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#include "../audio/i2s_codec.h"

#include <math.h>

enum
{
	DmaBufferSize = 4096,
	DmaPollIntervalNs = 1000000,
};

static void streamSchedule(Esp32s3I2SStream* stream);

static void dmaIrq(DmaConfigState* channel)
{
	channel->int_state.st = channel->int_state.raw & channel->int_state.ena;
	qemu_set_irq(channel->int_state.irq, channel->int_state.st != 0);
}

static DmaConfigState* streamChannel(Esp32s3I2SStream* stream)
{
	ESPGdmaState* gdma = stream->i2s->gdma;
	if (!gdma) return NULL;
	
	const unsigned direction = stream->direction;
	const unsigned shift = direction == ESP_GDMA_IN_IDX;
	const uint32_t start = 1U << (21 + shift), restart = start << 1;
	const uint32_t stop = start >> 1, park = start << 2;
	
	const unsigned count = ESP_GDMA_GET_CLASS(gdma)->m_channel_count;
	for (unsigned i = 0; i < count; i++)
	{
		DmaConfigState* channel = &gdma->ch_conf[direction][i];
		
		/* Do not use esp_gdma_get_channel_periph(): its OR condition can
		 * return a running SPI channel instead of the requested peripheral. */
		if ((channel->peripheral & 63) != GDMA_I2S0)
		{
			continue;
		}
		if (channel->link & (start | restart))
		{
			stream->deadlineNs = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
			stream->channel = i;
			if ((channel->link & start) || !channel->desc_addr)
			{
				stream->descriptor = (ESP_GDMA_RAM_ADDR & 0xFFF00000) | (channel->link & 0xFFFFF);
			}
			else
			{
				stream->descriptor = channel->desc_addr;
			}
			stream->pendingValid = false;
			stream->rxEofBytes = 0;
			channel->link &= ~(start | restart | stop | park);
		}
		
		if (stream->channel != i)
		{
			continue;
		}
		if ((channel->conf0 & 1) || (channel->link & (stop | park)))
		{
			channel->link = (channel->link & ~stop) | park;
			stream->descriptor = 0;
			stream->pendingValid = false;
			return NULL;
		}
		return stream->descriptor ? channel : NULL;
	}
	return NULL;
}

static void dmaError(Esp32s3I2SStream* stream, DmaConfigState* channel)
{
	const bool rx = stream->direction == ESP_GDMA_IN_IDX;
	qemu_log_mask(LOG_GUEST_ERROR,
	              "esp32s3-i2s: %s GDMA descriptor error at 0x%08x\n",
	              rx ? "RX" : "TX", stream->descriptor);
	
	channel->int_state.raw |= rx ? R_GDMA_INTERRUPT_IN_DSCR_ERR_MASK : R_GDMA_INTERRUPT_OUT_DSCR_ERR_MASK;
	channel->link |= 1U << (rx ? 24 : 23);
	stream->descriptor = 0;
	stream->pendingValid = false;
	dmaIrq(channel);
}

static void streamSchedule(Esp32s3I2SStream* stream)
{
	Esp32s3I2S* i2s = stream->i2s;
	const double periodNs = esp32s3I2sBytePeriodNs(i2s, stream->direction);
	const int64_t nowNs = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	esp32s3I2sUpdateAudio(i2s, stream->direction);
	
	if (!periodNs)
	{
		timer_del(stream->timer);
		stream->pendingValid = false;
		stream->deadlineNs = nowNs;
		return;
	}
	
	DmaConfigState* channel = streamChannel(stream);
	if (!channel)
	{
		stream->deadlineNs = nowNs;
		/* GDMA register writes have no peripheral notification hook. Poll
		 * only while I2S has a running clock, without manufacturing EOFs. */
		timer_mod(stream->timer, nowNs + DmaPollIntervalNs);
		return;
	}
	if (stream->pendingValid) return;
	
	uint32_t* descriptor = stream->pending;
	if ((stream->descriptor & 3) ||
	    address_space_read(&i2s->gdma->dma_as, stream->descriptor,
	                       MEMTXATTRS_UNSPECIFIED, descriptor, sizeof(stream->pending)) != MEMTX_OK)
	{
		dmaError(stream, channel);
		return;
	}
	
	const uint32_t flags = le32_to_cpu(descriptor[0]);
	const unsigned capacity = flags & 0xFFF;
	if (stream->direction == ESP_GDMA_IN_IDX)
	{
		const unsigned eofBytes = i2s->registers[RxEof / 4];
		stream->bytes = MIN(capacity, eofBytes - MIN(stream->rxEofBytes, eofBytes));
	}
	else
	{
		stream->bytes = (flags >> 12) & 0xFFF;
	}
	if (!stream->bytes || stream->bytes > capacity ||
	    ((channel->conf1 & (1 << 12)) && !(flags & (1U << 31))))
	{
		dmaError(stream, channel);
		return;
	}
	
	stream->pendingValid = true;
	channel->state = stream->descriptor & 0x3FFFF;
	/* Keep the sample clock steady when the host dispatches a timer late. */
	stream->deadlineNs += MAX((int64_t) ceil(stream->bytes * periodNs), 1);
	timer_mod(stream->timer, stream->deadlineNs);
}

static void streamComplete(void* opaque)
{
	Esp32s3I2SStream* stream = opaque;
	Esp32s3I2S* i2s = stream->i2s;
	DmaConfigState* channel = streamChannel(stream);
	
	if (!channel || !stream->pendingValid || !esp32s3I2sBytePeriodNs(i2s, stream->direction))
	{
		streamSchedule(stream);
		return;
	}
	
	uint8_t samples[DmaBufferSize] = { 0 };
	uint32_t flags = le32_to_cpu(stream->pending[0]);
	const uint32_t buffer = le32_to_cpu(stream->pending[1]);
	const uint32_t next = le32_to_cpu(stream->pending[2]);
	
	const bool rx = stream->direction == ESP_GDMA_IN_IDX;
	MemTxResult result;
	if (rx)
	{
		esp32s3I2sTransferAudio(i2s, stream->direction, samples, stream->bytes);
		result = address_space_write(&i2s->gdma->dma_as, buffer, MEMTXATTRS_UNSPECIFIED, samples, stream->bytes);
	}
	else
	{
		result = address_space_read(&i2s->gdma->dma_as, buffer, MEMTXATTRS_UNSPECIFIED, samples, stream->bytes);
	}
	if (result != MEMTX_OK)
	{
		dmaError(stream, channel);
		return;
	}
	
	if (!rx) esp32s3I2sTransferAudio(i2s, stream->direction, samples, stream->bytes);
	
	bool eof = flags & (1U << 30);
	if (rx)
	{
		stream->rxEofBytes += stream->bytes;
		eof = stream->rxEofBytes == i2s->registers[RxEof / 4];
		if (eof)
		{
			stream->rxEofBytes = 0;
		}
		flags = (flags & ~0xD0FFF000U) | (stream->bytes << 12) | (eof ? 1U << 30 : 0);
	}
	else if (channel->conf0 & R_GDMA_OUT_CONF0_AUTO_WRBACK_MASK)
	{
		flags &= ~(1U << 31); /* Return descriptor ownership to the CPU. */
	}
	
	if (rx || (channel->conf0 & R_GDMA_OUT_CONF0_AUTO_WRBACK_MASK))
	{
		const uint32_t written = cpu_to_le32(flags);
		if (address_space_write(&i2s->gdma->dma_as, stream->descriptor,
		                        MEMTXATTRS_UNSPECIFIED, &written, sizeof(written)) != MEMTX_OK)
		{
			dmaError(stream, channel);
			return;
		}
	}
	
	channel->bfr_bfr_desc_addr = channel->bfr_desc_addr;
	channel->bfr_desc_addr = stream->descriptor;
	channel->desc_addr = next;
	channel->int_state.raw |= 1; /* IN_DONE / OUT_DONE */
	if (eof)
	{
		channel->suc_eof_desc_addr = stream->descriptor;
		channel->int_state.raw |= 2; /* IN_SUC_EOF / OUT_EOF */
	}
	if (!next)
	{
		channel->link |= 1U << (rx ? 24 : 23); /* PARK at end of list. */
		channel->int_state.raw |= rx ? R_GDMA_INTERRUPT_IN_DSCR_EMPTY_MASK : R_GDMA_INTERRUPT_OUT_TOTAL_EOF_MASK;
	}
	
	stream->descriptor = next;
	stream->pendingValid = false;
	dmaIrq(channel);
	streamSchedule(stream);
}

static uint64_t i2sRead(void* opaque, hwaddr offset, unsigned size)
{
	const Esp32s3I2S* i2s = opaque;
	
	switch (offset)
	{
		case IntSt:
			return i2s->registers[IntRaw / 4] & i2s->registers[IntEna / 4];
		case IntClr:
			return 0;
		case State:
			return !i2s->stream[ESP_GDMA_OUT_IDX].pendingValid;
		default:
			return offset < sizeof(i2s->registers) ? i2s->registers[offset / 4] : 0;
	}
}

static void i2sWrite(void* opaque, hwaddr offset, uint64_t value, unsigned size)
{
	Esp32s3I2S* i2s = opaque;
	
	switch (offset)
	{
		case RxConf:
		case TxConf: {
			/* Reset and START are ordinary R/W control bits. Reset affects the
			 * channel/FIFO, not its clock, format or configuration registers. */
			i2s->registers[offset / 4] = value & ~Update;
			if (value & (Reset | FifoReset))
			{
				Esp32s3I2SStream* stream = &i2s->stream[(offset - RxConf) / 4];
				timer_del(stream->timer);
				stream->descriptor = 0;
				stream->pendingValid = false;
				stream->rxEofBytes = 0;
			}
			break;
		}
		case IntClr: {
			i2s->registers[IntRaw / 4] &= ~(value & 15);
			break;
		}
		case IntEna: {
			i2s->registers[offset / 4] = value & 15;
			break;
		}
		case IntRaw:
		case IntSt:
		case State:
			break;
		case Date: {
			i2s->registers[offset / 4] = value & 0x0FFFFFFF;
			break;
		}
		default: {
			if ((offset >= 0x28 && offset <= 0x44) ||
			    (offset >= 0x50 && offset <= 0x68))
			{
				i2s->registers[offset / 4] = value;
			}
			break;
		}
	}
	
	if (offset >= RxConf && offset <= RxEof)
	{
		for (unsigned direction = 0; direction < ESP_GDMA_CONF_COUNT; direction++)
		{
			/* IDF changes formats/clocks with the channel stopped. At
			 * descriptor granularity an in-flight update restarts
			 * that descriptor's interval; it does not produce a partial EOF. */
			i2s->stream[direction].pendingValid = false;
			i2s->stream[direction].deadlineNs = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
			streamSchedule(&i2s->stream[direction]);
		}
	}
}

static void i2sReset(DeviceState* dev)
{
	Esp32s3I2S* i2s = ESP32S3_I2S(dev);
	
	/* Documented S3 reset values, including clock divisors and slot widths. */
	static const uint32_t Defaults[0x84 / 4] = {
		[RxConf / 4] = 0x00009600,
		[TxConf / 4] = 0x0000B200,
		[RxConf1 / 4] = 0x2F3DE300,
		[0x2C / 4] = 0x6F3DE300,
		[RxClk / 4] = 2,
		[TxClk / 4] = 2,
		[RxDiv / 4] = 0x200,
		[0x3C / 4] = 0x200,
		[0x40 / 4] = 0x004AA004,
		[0x44 / 4] = 0x03F783C0,
		[RxTdm / 4] = 0xFFFF,
		[0x54 / 4] = 0xFFFF,
		[0x60 / 4] = 0x810,
		[RxEof / 4] = 0x40,
		[Date / 4] = 0x02009070,
	};
	
	memcpy(i2s->registers, Defaults, sizeof(i2s->registers));
	for (unsigned direction = 0; direction < ESP_GDMA_CONF_COUNT; direction++)
	{
		Esp32s3I2SStream* stream = &i2s->stream[direction];
		timer_del(stream->timer);
		stream->channel = UINT_MAX;
		stream->descriptor = stream->rxEofBytes = 0;
		stream->pendingValid = false;
		esp32s3I2sUpdateAudio(i2s, direction);
	}
}

static const MemoryRegionOps I2sOps = {
	.read = i2sRead,
	.write = i2sWrite,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = { .min_access_size = 4, .max_access_size = 4 },
	.impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void instanceInit(Object* obj)
{
	Esp32s3I2S* i2s = ESP32S3_I2S(obj);
	for (unsigned direction = 0; direction < ESP_GDMA_CONF_COUNT; direction++)
	{
		i2s->stream[direction].i2s = i2s;
		i2s->stream[direction].direction = direction;
		i2s->stream[direction].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, streamComplete, &i2s->stream[direction]);
	}
	i2sReset(DEVICE(obj));
	
	memory_region_init_io(&i2s->mmio, obj, &I2sOps, i2s, TYPE_ESP32S3_I2S, 0x100);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &i2s->mmio);
}

static void instanceFinalize(Object* obj)
{
	Esp32s3I2S* i2s = ESP32S3_I2S(obj);
	for (unsigned direction = 0; direction < ESP_GDMA_CONF_COUNT; direction++)
	{
		timer_free(i2s->stream[direction].timer);
	}
}

static Property Properties[] = {
	DEFINE_PROP_LINK("gdma", Esp32s3I2S, gdma, TYPE_ESP_GDMA, ESPGdmaState*),
	DEFINE_PROP_LINK("codec", Esp32s3I2S, codec, TYPE_I2S_CODEC, Object*),
	DEFINE_PROP_END_OF_LIST(),
};

static void classInit(ObjectClass* klass, void* data)
{
	DeviceClass* device = DEVICE_CLASS(klass);
	device_class_set_props(device, Properties);
	device_class_set_legacy_reset(device, i2sReset);
}

static const TypeInfo DeviceTypes[] = {
	{
		.name = TYPE_ESP32S3_I2S,
		.parent = TYPE_SYS_BUS_DEVICE,
		.instance_size = sizeof(Esp32s3I2S),
		.instance_init = instanceInit,
		.instance_finalize = instanceFinalize,
		.class_init = classInit,
	},
};

DEFINE_TYPES(DeviceTypes)
