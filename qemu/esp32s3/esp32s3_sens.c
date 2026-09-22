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

/* RTC ADC single conversions. Unconnected analog inputs read as ground;
 * analog calibration offsets, conversion timing and ADC DMA are not modeled. */

#include "qemu/osdep.h"
#include "esp32s3_sens.h"

#include "hw/sysbus.h"
#include "qemu/module.h"

OBJECT_DECLARE_SIMPLE_TYPE(Esp32s3Sens, ESP32S3_SENS)

enum
{
	Reader1Control = 0x00,
	Reader1Status = 0x04,
	Measure1Control = 0x0C,
	Reader2Control = 0x24,
	Reader2Status = 0x28,
	Measure2Control = 0x30,
	SlaveAddress = 0x40,
	
	DataMask = 0xFFFF,
	ConversionDone = 1 << 16,
	ConversionStart = 1 << 17,
	SoftwareStart = 1 << 18,
};

struct Esp32s3Sens
{
	SysBusDevice parentObj;
	MemoryRegion mmio;
	uint32_t registers[0x100 / 4];
};

static uint64_t sensRead(void* opaque, hwaddr address, unsigned size)
{
	return ((Esp32s3Sens*) opaque)->registers[address / 4];
}

static void sensWrite(void* opaque, hwaddr address, uint64_t value, unsigned size)
{
	Esp32s3Sens* sens = opaque;
	uint32_t* reg = &sens->registers[address / 4];
	if (address == Reader1Status || address == Reader2Status) return;
	if (address == SlaveAddress) value &= ~(0xFFU << 22); // Read-only measurement FSM: idle.
	if (address != Measure1Control && address != Measure2Control)
	{
		*reg = value;
		return;
	}
	
	const uint32_t previous = *reg;
	*reg = (value & ~(DataMask | ConversionDone)) | (previous & (DataMask | ConversionDone));
	if (!(value & SoftwareStart) || !(value & ConversionStart) || (previous & ConversionStart)) return;
	
	const bool second = address == Measure2Control;
	const uint32_t reader = sens->registers[(second ? Reader2Control : Reader1Control) / 4];
	const bool invert = (reader & (1U << (second ? 29 : 28))) != 0;
	*reg = (*reg & ~DataMask) | ConversionDone | (invert ? 0xFFF : 0);
}

static const MemoryRegionOps SensOps = {
	.read = sensRead,
	.write = sensWrite,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void instanceInit(Object* obj)
{
	Esp32s3Sens* sens = ESP32S3_SENS(obj);
	memory_region_init_io(&sens->mmio, obj, &SensOps, sens, TYPE_ESP32S3_SENS, sizeof(sens->registers));
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &sens->mmio);
}

static void sensReset(DeviceState* dev)
{
	Esp32s3Sens* sens = ESP32S3_SENS(dev);
	memset(sens->registers, 0, sizeof(sens->registers));
	sens->registers[Reader1Control / 4] = 0x20040002;
	sens->registers[Reader2Control / 4] = 0x40050002;
}

static void classInit(ObjectClass* klass, void* data)
{
	device_class_set_legacy_reset(DEVICE_CLASS(klass), sensReset);
}

static const TypeInfo DeviceTypes[] = {
	{
		.name = TYPE_ESP32S3_SENS,
		.parent = TYPE_SYS_BUS_DEVICE,
		.instance_size = sizeof(Esp32s3Sens),
		.instance_init = instanceInit,
		.class_init = classInit,
	},
};

DEFINE_TYPES(DeviceTypes)
