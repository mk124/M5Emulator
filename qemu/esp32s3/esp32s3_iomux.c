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

/* IO MUX registers and digital pull resistors; peripheral function routing is not modeled. */

#include "qemu/osdep.h"
#include "esp32s3_iomux.h"

#include "hw/irq.h"
#include "hw/sysbus.h"
#include "qemu/module.h"

#include "esp32s3_gpio.h"

OBJECT_DECLARE_SIMPLE_TYPE(Esp32s3Iomux, ESP32S3_IOMUX)

struct Esp32s3Iomux
{
	SysBusDevice parentObj;
	MemoryRegion mmio;
	uint32_t registers[0x2000 / 4];
	qemu_irq pull[Esp32s3GpioPinCount];
};

static uint64_t iomuxRead(void* opaque, hwaddr address, unsigned size)
{
	return ((Esp32s3Iomux*) opaque)->registers[address / 4];
}

static void iomuxWrite(void* opaque, hwaddr address, uint64_t value, unsigned size)
{
	Esp32s3Iomux* iomux = opaque;
	iomux->registers[address / 4] = value;
	if (address >= 4 && address <= Esp32s3GpioPinCount * 4)
	{
		qemu_set_irq(iomux->pull[address / 4 - 1], (value >> 7) & 3);
	}
}

static const MemoryRegionOps IomuxOps = {
	.read = iomuxRead,
	.write = iomuxWrite,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void instanceInit(Object* obj)
{
	Esp32s3Iomux* iomux = ESP32S3_IOMUX(obj);
	memory_region_init_io(&iomux->mmio, obj, &IomuxOps, iomux, TYPE_ESP32S3_IOMUX, 0x2000);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &iomux->mmio);
	qdev_init_gpio_out_named(DEVICE(obj), iomux->pull, "pull", ARRAY_SIZE(iomux->pull));
}

static void iomuxReset(DeviceState* dev)
{
	Esp32s3Iomux* iomux = ESP32S3_IOMUX(dev);
	memset(iomux->registers, 0, sizeof(iomux->registers));
	for (unsigned pin = 0; pin < ARRAY_SIZE(iomux->pull); pin++) qemu_set_irq(iomux->pull[pin], 0);
}

static void classInit(ObjectClass* klass, void* data)
{
	device_class_set_legacy_reset(DEVICE_CLASS(klass), iomuxReset);
}

static const TypeInfo DeviceTypes[] = {
	{
		.name = TYPE_ESP32S3_IOMUX,
		.parent = TYPE_SYS_BUS_DEVICE,
		.instance_size = sizeof(Esp32s3Iomux),
		.instance_init = instanceInit,
		.class_init = classInit,
	},
};

DEFINE_TYPES(DeviceTypes)
