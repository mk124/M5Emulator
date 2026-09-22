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

/* ESP32-S3 SPI master: byte-aligned MOSI phases, FIFO/GDMA and transaction IRQ.
 * Hardware CS outputs route through the GPIO matrix. MISO, bit-level timing
 * and other transfer formats are not modeled. */

#include "qemu/osdep.h"
#include "esp32s3_spi.h"

#include "exec/address-spaces.h"
#include "hw/dma/esp_gdma.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "hw/sysbus.h"
#include "qemu/error-report.h"
#include "qemu/module.h"

enum
{
	SpiCmd = 0x00,
	SpiAddr = 0x04,
	SpiUser = 0x10,
	SpiUser1 = 0x14,
	SpiUser2 = 0x18,
	SpiDlen = 0x1C,
	SpiMisc = 0x20,
	SpiDma = 0x30,
	SpiEna = 0x34,
	SpiClr = 0x38,
	SpiRaw = 0x3C,
	SpiSt = 0x40,
	SpiSet = 0x44,
	SpiW0 = 0x98,
};

enum { SpiFifoCapacity = 64 };

#define SPI_CMD_UPDATE (1U << 23)
#define SPI_CMD_USR (1U << 24)

#define SPI_USER_MOSI_HIGH_PART (1U << 25)
#define SPI_USER_MOSI (1U << 27)
#define SPI_USER_ADDRESS (1U << 30)
#define SPI_USER_COMMAND (1U << 31)

#define SPI_DMA_TX_ENABLE (1U << 28)
#define SPI_CS_KEEP_ACTIVE (1U << 30)
#define SPI_TRANS_DONE (1U << 12)

OBJECT_DECLARE_SIMPLE_TYPE(Esp32s3Spi, ESP32S3_SPI_CONTROLLER)

struct Esp32s3Spi
{
	SysBusDevice parentObj;
	MemoryRegion mmio;
	uint32_t registers[0x100 / 4];
	qemu_irq irq;
	qemu_irq chipSelect[Esp32s3SpiCsCount];
	SSIBus* bus;
	
	ESPGdmaState* gdma;
	uint32_t dmaPeripheral;
	uint32_t dmaChannel, dmaDescriptor, dmaOffset;
};

static void spiInterrupt(Esp32s3Spi* spi)
{
	qemu_set_irq(spi->irq, (spi->registers[SpiRaw / 4] & spi->registers[SpiEna / 4]) != 0);
}

/* Retain the GDMA chain position across successive SPI transactions. */
static bool spiDmaRead(Esp32s3Spi* spi, uint8_t* buffer, size_t size)
{
	if (!spi->gdma) return false;
	
	const unsigned count = ESP_GDMA_GET_CLASS(spi->gdma)->m_channel_count;
	DmaConfigState* channel = NULL;
	for (unsigned i = 0; i < count; i++)
	{
		DmaConfigState* candidate = &spi->gdma->ch_conf[ESP_GDMA_OUT_IDX][i];
		if (candidate->peripheral != spi->dmaPeripheral) continue;
		if (candidate->link & (1U << 21))
		{
			spi->dmaChannel = i;
			spi->dmaDescriptor = 0x3FC00000 | (candidate->link & 0xFFFFF);
			spi->dmaOffset = 0;
			candidate->link &= ~(1U << 21);
		}
		if (spi->dmaChannel == i && spi->dmaDescriptor)
		{
			channel = candidate;
			break;
		}
	}
	if (!channel) return false;
	
	while (size)
	{
		uint32_t descriptor[3];
		if (address_space_read(&spi->gdma->dma_as, spi->dmaDescriptor,
		                       MEMTXATTRS_UNSPECIFIED, descriptor, sizeof(descriptor)) != MEMTX_OK)
		{
			return false;
		}
		
		const uint32_t flags = le32_to_cpu(descriptor[0]);
		const uint32_t length = (flags >> 12) & 0xFFF;
		const uint32_t address = le32_to_cpu(descriptor[1]);
		const uint32_t next = le32_to_cpu(descriptor[2]);
		if (length <= spi->dmaOffset ||
		    ((channel->conf1 & (1U << 12)) && !(flags & (1U << 31)))) return false;
		
		const size_t chunkSize = MIN(size, length - spi->dmaOffset);
		if (address_space_read(&spi->gdma->dma_as, address + spi->dmaOffset,
		                       MEMTXATTRS_UNSPECIFIED, buffer, chunkSize) != MEMTX_OK)
		{
			return false;
		}
		buffer += chunkSize;
		size -= chunkSize;
		spi->dmaOffset += chunkSize;
		
		if (spi->dmaOffset == length)
		{
			if (channel->conf0 & R_GDMA_OUT_CONF0_AUTO_WRBACK_MASK)
			{
				descriptor[0] = cpu_to_le32(flags & ~(1U << 31));
				if (address_space_write(&spi->gdma->dma_as, spi->dmaDescriptor,
				                        MEMTXATTRS_UNSPECIFIED, descriptor, 4) != MEMTX_OK)
				{
					return false;
				}
			}
			
			channel->bfr_bfr_desc_addr = channel->bfr_desc_addr;
			channel->bfr_desc_addr = spi->dmaDescriptor;
			channel->desc_addr = next;
			channel->state = spi->dmaDescriptor & 0x3FFFF;
			channel->int_state.raw |= 1;
			if (flags & (1U << 30))
			{
				channel->suc_eof_desc_addr = spi->dmaDescriptor;
				channel->int_state.raw |= 2;
			}
			spi->dmaDescriptor = next;
			spi->dmaOffset = 0;
			if (!next)
			{
				channel->int_state.raw |= 8;
				channel->link |= 1U << 23;
			}
		}
	}
	
	channel->int_state.st = channel->int_state.raw & channel->int_state.ena;
	qemu_set_irq(channel->int_state.irq, channel->int_state.st != 0);
	return true;
}

static void sendPhase(Esp32s3Spi* spi, uint32_t value, unsigned bits)
{
	/* Address values are left-aligned; command values are aligned by the caller. */
	for (unsigned sent = 0; sent < bits; sent += 8)
	{
		ssi_transfer(spi->bus, (value >> (24 - sent)) & 0xFF);
	}
}

static void spiChipSelect(Esp32s3Spi* spi, bool active)
{
	const uint32_t misc = spi->registers[SpiMisc / 4];
	for (unsigned cs = 0; cs < Esp32s3SpiCsCount; cs++)
	{
		const bool selected = active && !(misc & (1U << cs));
		const bool inverted = (misc & (1U << (cs + 7))) != 0;
		qemu_set_irq(spi->chipSelect[cs], !selected ^ inverted);
	}
}

static void spiTransfer(Esp32s3Spi* spi)
{
	spiChipSelect(spi, true);
	const uint32_t user = spi->registers[SpiUser / 4];
	const unsigned commandBits = (user & SPI_USER_COMMAND) ? ((spi->registers[SpiUser2 / 4] >> 28) & 15) + 1 : 0;
	const unsigned addressBits = (user & SPI_USER_ADDRESS) ? ((spi->registers[SpiUser1 / 4] >> 27) & 31) + 1 : 0;
	if ((commandBits % 8) || (addressBits % 8))
	{
		warn_report("ESP32-S3 SPI: only byte-aligned command/address phases are supported");
		goto complete;
	}
	if (commandBits) sendPhase(spi, spi->registers[SpiUser2 / 4] << (32 - commandBits), commandBits);
	if (addressBits) sendPhase(spi, spi->registers[SpiAddr / 4], addressBits);
	
	/* MS_DATA_BITLEN is an 18-bit field in the S3 SPI controller. */
	size_t length = (user & SPI_USER_MOSI) ? ((spi->registers[SpiDlen / 4] & 0x3FFFF) + 8) / 8 : 0;
	uint8_t fifo[SpiFifoCapacity];
	uint8_t* allocated = NULL;
	const uint8_t* data = fifo;
	if (length && (spi->registers[SpiDma / 4] & SPI_DMA_TX_ENABLE))
	{
		allocated = g_malloc0(length);
		data = allocated;
		if (!spiDmaRead(spi, allocated, length))
		{
			warn_report("ESP32-S3 SPI: DMA transfer failed");
			length = 0;
		}
	}
	else
	{
		const unsigned start = (user & SPI_USER_MOSI_HIGH_PART) ? 8 : 0;
		length = MIN(length, sizeof(fifo) - start * 4);
		for (size_t i = 0; i < length; i++)
		{
			fifo[i] = spi->registers[SpiW0 / 4 + start + i / 4] >> ((i % 4) * 8);
		}
	}
	
	for (size_t i = 0; i < length; i++) ssi_transfer(spi->bus, data[i]);
	g_free(allocated);

complete:
	spiChipSelect(spi, (spi->registers[SpiMisc / 4] & SPI_CS_KEEP_ACTIVE) != 0);
	spi->registers[SpiCmd / 4] = 0;
	spi->registers[SpiRaw / 4] |= SPI_TRANS_DONE;
	spiInterrupt(spi);
}

static uint64_t spiRead(void* opaque, hwaddr address, unsigned size)
{
	const Esp32s3Spi* spi = opaque;
	if (address == SpiSt) return spi->registers[SpiRaw / 4] & spi->registers[SpiEna / 4];
	return spi->registers[address / 4];
}

static void spiWrite(void* opaque, hwaddr address, uint64_t value, unsigned size)
{
	Esp32s3Spi* spi = opaque;
	
	if (address == SpiClr) spi->registers[SpiRaw / 4] &= ~value;
	else if (address == SpiSet) spi->registers[SpiRaw / 4] |= value;
	else
	{
		spi->registers[address / 4] = value;
	}
	
	if (address == SpiCmd)
	{
		spi->registers[SpiCmd / 4] &= ~SPI_CMD_UPDATE; /* configuration update completes */
		if (value & SPI_CMD_USR) spiTransfer(spi);
	}
	spiInterrupt(spi);
}

static void spiReset(DeviceState* dev)
{
	Esp32s3Spi* spi = ESP32S3_SPI_CONTROLLER(dev);
	memset(spi->registers, 0, sizeof(spi->registers));
	spi->registers[SpiMisc / 4] = 0x3F;
	spiChipSelect(spi, false);
	spi->dmaChannel = UINT32_MAX;
	spi->dmaDescriptor = spi->dmaOffset = 0;
	spiInterrupt(spi);
}

static const MemoryRegionOps SpiOps = {
	.read = spiRead,
	.write = spiWrite,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void instanceInit(Object* obj)
{
	Esp32s3Spi* spi = ESP32S3_SPI_CONTROLLER(obj);
	spi->bus = ssi_create_bus(DEVICE(obj), "spi");
	memory_region_init_io(&spi->mmio, obj, &SpiOps, spi, TYPE_ESP32S3_SPI_CONTROLLER, 0x100);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &spi->mmio);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &spi->irq);
	qdev_init_gpio_out_named(DEVICE(obj), spi->chipSelect, "cs", Esp32s3SpiCsCount);
	spiReset(DEVICE(obj));
}

static Property Properties[] = {
	DEFINE_PROP_LINK("gdma", Esp32s3Spi, gdma, TYPE_ESP_GDMA, ESPGdmaState*),
	DEFINE_PROP_UINT32("dma-peripheral", Esp32s3Spi, dmaPeripheral, GDMA_SPI2),
	DEFINE_PROP_END_OF_LIST(),
};

static void classInit(ObjectClass* klass, void* data)
{
	DeviceClass* device = DEVICE_CLASS(klass);
	device_class_set_props(device, Properties);
	device_class_set_legacy_reset(device, spiReset);
}

static const TypeInfo DeviceTypes[] = {
	{
		.name = TYPE_ESP32S3_SPI_CONTROLLER,
		.parent = TYPE_SYS_BUS_DEVICE,
		.instance_size = sizeof(Esp32s3Spi),
		.instance_init = instanceInit,
		.class_init = classInit,
	},
};

DEFINE_TYPES(DeviceTypes)
