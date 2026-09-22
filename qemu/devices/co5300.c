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

/* CO5300 power/reset inputs, write-only SPI/QSPI, RGB565 GRAM and 60 Hz TE.
 * Readback, other pixel formats, line-level TE and electrical timing are not modeled. */

#include "qemu/osdep.h"
#include "co5300.h"

#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"

enum
{
	GramWidth = 480,
	GramHeight = 480,
	TeIntervalNs = 16666667,
	PowerOnDelayNs = 10000000,
	ResetRecoveryNs = 5000000,
	SleepOutRecoveryNs = 120000000,
};

enum
{
	SoftwareReset = 0x01,
	SleepIn = 0x10,
	SleepOut = 0x11,
	DisplayOff = 0x28,
	DisplayOn = 0x29,
	ColumnAddressSet = 0x2A,
	RowAddressSet = 0x2B,
	MemoryWrite = 0x2C,
	TearingOff = 0x34,
	TearingOn = 0x35,
	MemoryWriteContinue = 0x3C,
	WriteBrightness = 0x51,
};

OBJECT_DECLARE_SIMPLE_TYPE(Co5300, CO5300)

struct Co5300
{
	SSIPeripheral parentObj;
	uint16_t gram[GramWidth * GramHeight];
	uint16_t x0, x1, y0, y1, x, y;
	uint8_t pixelHi;
	bool halfPixel;
	
	uint8_t command, params[4], paramCount;
	uint8_t header[4], headerCount;
	uint8_t brightness;
	bool displayOn, sleeping, teEnabled;
	
	bool powered, resetAsserted, resetWhileAwake;
	int64_t readyAtNs, sleepOutAtNs;
	
	bool simulateBrightness;
	uint16_t width, height, xOffset, yOffset;
	uint64_t updates, activeRefreshes;
	uint32_t completedWindows, previousWindows;
	
	qemu_irq te;
	QEMUTimer* timer;
	uint64_t nextTeNs;
};

static void scheduleRefresh(Co5300* device)
{
	if (device->powered && !device->resetAsserted && !device->sleeping)
	{
		if (!timer_pending(device->timer))
		{
			device->nextTeNs = MAX(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), device->readyAtNs) + TeIntervalNs;
			timer_mod(device->timer, device->nextTeNs);
		}
	}
	else
	{
		timer_del(device->timer);
		qemu_set_irq(device->te, 0);
		device->previousWindows = device->completedWindows;
	}
}

static void resetRegisters(Co5300* device)
{
	/* CO5300 RAMWR defaults preserve GRAM across both hardware and software reset. */
	device->x0 = device->y0 = device->x = device->y = 0;
	device->x1 = GramWidth - 1;
	device->y1 = GramHeight - 1;
	
	device->command = device->paramCount = device->headerCount = 0;
	device->halfPixel = false;
	
	device->displayOn = device->teEnabled = false;
	device->sleeping = true;
	device->brightness = 0;
	device->updates++;
	scheduleRefresh(device);
}

static void powerInput(void* opaque, int line, int level)
{
	Co5300* device = opaque;
	const bool powered = level != 0;
	if (device->powered == powered) return;
	device->powered = powered;
	
	/* Power-on RAM is unspecified; choose deterministic black contents. */
	if (powered) memset(device->gram, 0, sizeof(device->gram));
	resetRegisters(device);
	device->resetWhileAwake = false;
	device->readyAtNs = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PowerOnDelayNs;
	device->sleepOutAtNs = device->readyAtNs;
}

static void resetInput(void* opaque, int line, int level)
{
	Co5300* device = opaque;
	const bool asserted = level == 0;
	if (device->resetAsserted == asserted) return;
	device->resetAsserted = asserted;
	
	if (asserted)
	{
		device->resetWhileAwake = !device->sleeping;
		resetRegisters(device);
	}
	else
	{
		const int64_t nowNs = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
		device->readyAtNs = MAX(device->readyAtNs, nowNs + ResetRecoveryNs);
		device->sleepOutAtNs = MAX(device->readyAtNs, nowNs + (device->resetWhileAwake ? SleepOutRecoveryNs : ResetRecoveryNs));
	}
}

static void panelCommand(Co5300* device, uint8_t command)
{
	if (command == SoftwareReset)
	{
		const int64_t nowNs = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
		device->readyAtNs = nowNs + ResetRecoveryNs;
		device->sleepOutAtNs = nowNs + (device->sleeping ? ResetRecoveryNs : SleepOutRecoveryNs);
		resetRegisters(device);
		return;
	}
	
	device->command = command;
	device->paramCount = 0;
	device->halfPixel = false;
	
	if (command == MemoryWrite)
	{
		device->x = device->x0;
		device->y = device->y0;
	}
	
	if (command == DisplayOff || command == DisplayOn)
	{
		device->displayOn = command == DisplayOn;
		device->updates++;
	}
	
	if (command == SleepIn || command == SleepOut)
	{
		if (command == SleepOut && qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < device->sleepOutAtNs) return;
		device->sleeping = command == SleepIn;
		device->updates++;
		scheduleRefresh(device);
	}
	
	if (command == TearingOff || command == TearingOn)
	{
		device->teEnabled = command == TearingOn;
	}
}

static void panelData(Co5300* device, uint8_t value)
{
	if (device->command == ColumnAddressSet || device->command == RowAddressSet)
	{
		if (device->paramCount >= 4) return;
		device->params[device->paramCount++] = value;
		if (device->paramCount == 4)
		{
			const uint16_t start = (device->params[0] << 8) | device->params[1];
			const uint16_t end = (device->params[2] << 8) | device->params[3];
			if (device->command == ColumnAddressSet)
			{
				device->x0 = start;
				device->x1 = end;
			}
			else
			{
				device->y0 = start;
				device->y1 = end;
			}
		}
	}
	else if (device->command == WriteBrightness && device->paramCount == 0)
	{
		device->brightness = value;
		device->paramCount++;
		device->updates++;
	}
	else if (device->command == MemoryWrite || device->command == MemoryWriteContinue)
	{
		if (!device->halfPixel)
		{
			device->pixelHi = value;
			device->halfPixel = true;
			return;
		}
		
		if (device->x < GramWidth && device->y < GramHeight)
		{
			device->gram[device->y * GramWidth + device->x] = (device->pixelHi << 8) | value;
			device->updates++;
		}
		device->halfPixel = false;
		
		if (++device->x <= device->x1) return;
		device->x = device->x0;
		if (++device->y <= device->y1) return;
		device->y = device->y0;
		
		if (device->x0 <= device->x1 && device->x1 < GramWidth &&
		    device->y0 <= device->y1 && device->y1 < GramHeight &&
		    device->x0 < device->xOffset + device->width && device->x1 >= device->xOffset &&
		    device->y0 < device->yOffset + device->height && device->y1 >= device->yOffset)
		{
			device->completedWindows++;
		}
	}
}

static int selectChanged(SSIPeripheral* peripheral, bool level)
{
	Co5300* device = CO5300(peripheral);
	if (level) device->headerCount = 0;
	return 0;
}

static uint32_t transfer(SSIPeripheral* peripheral, uint32_t value)
{
	Co5300* device = CO5300(peripheral);
	if (!device->powered || device->resetAsserted) return 0;
	if (device->readyAtNs)
	{
		if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < device->readyAtNs) return 0;
		device->readyAtNs = 0;
	}
	
	const uint8_t byte = value;
	if (device->headerCount < sizeof(device->header))
	{
		device->header[device->headerCount++] = byte;
		if (device->headerCount == sizeof(device->header)) panelCommand(device, device->header[2]);
	}
	else
	{
		panelData(device, byte);
	}
	return 0;
}

static void refresh(void* opaque)
{
	Co5300* device = opaque;
	const int64_t nowNs = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	if (device->completedWindows != device->previousWindows)
	{
		device->activeRefreshes++;
		device->previousWindows = device->completedWindows;
	}
	device->nextTeNs += ((nowNs - device->nextTeNs) / TeIntervalNs + 1) * TeIntervalNs;
	
	if (device->teEnabled) qemu_irq_pulse(device->te);
	timer_mod(device->timer, device->nextTeNs);
}

uint64_t co5300Updates(DeviceState* dev)
{
	return CO5300(dev)->updates;
}

uint32_t co5300CompletedWindows(DeviceState* dev)
{
	return CO5300(dev)->completedWindows;
}

uint64_t co5300ActiveRefreshes(DeviceState* dev)
{
	return CO5300(dev)->activeRefreshes;
}

void co5300SetBrightnessSimulation(DeviceState* dev, bool enabled)
{
	Co5300* device = CO5300(dev);
	if (device->simulateBrightness == enabled) return;
	device->simulateBrightness = enabled;
	device->updates++;
}

void co5300ReadPixels(DeviceState* dev, uint16_t* destination)
{
	const Co5300* device = CO5300(dev);
	const bool visible = device->powered && !device->resetAsserted && device->displayOn && !device->sleeping;
	const unsigned brightness = visible ? (device->simulateBrightness ? device->brightness : 255) : 0;
	
	for (unsigned y = 0; y < device->height; y++)
	{
		uint16_t* row = &destination[y * device->width];
		const uint16_t* source = &device->gram[(y + device->yOffset) * GramWidth + device->xOffset];
		if (brightness == 255)
		{
			memcpy(row, source, device->width * sizeof(*row));
		}
		else if (brightness == 0)
		{
			memset(row, 0, device->width * sizeof(*row));
		}
		else
		{
			/* Approximate luminance; GRAM retains the original color. */
			for (unsigned x = 0; x < device->width; x++)
			{
				const unsigned red = (source[x] >> 11) * brightness / 255;
				const unsigned green = ((source[x] >> 5) & 63) * brightness / 255;
				const unsigned blue = (source[x] & 31) * brightness / 255;
				row[x] = (red << 11) | (green << 5) | blue;
			}
		}
	}
}

static void realize(SSIPeripheral* peripheral, Error** errp)
{
	Co5300* device = CO5300(peripheral);
	if (!device->width || !device->height || device->xOffset + device->width > GramWidth || device->yOffset + device->height > GramHeight)
	{
		error_setg(errp, "CO5300 viewport must fit its %u x %u GRAM", GramWidth, GramHeight);
	}
}

static void instanceInit(Object* obj)
{
	Co5300* device = CO5300(obj);
	device->resetAsserted = true;
	device->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, refresh, device);
	qdev_init_gpio_in_named(DEVICE(obj), powerInput, "power", 1);
	qdev_init_gpio_in_named(DEVICE(obj), resetInput, "reset", 1);
	qdev_init_gpio_out_named(DEVICE(obj), &device->te, "te", 1);
	resetRegisters(device);
}

static void instanceFinalize(Object* obj)
{
	timer_free(CO5300(obj)->timer);
}

static Property Properties[] = {
	DEFINE_PROP_UINT16("width", Co5300, width, GramWidth),
	DEFINE_PROP_UINT16("height", Co5300, height, GramHeight),
	DEFINE_PROP_UINT16("x-offset", Co5300, xOffset, 0),
	DEFINE_PROP_UINT16("y-offset", Co5300, yOffset, 0),
	DEFINE_PROP_END_OF_LIST(),
};

static void classInit(ObjectClass* klass, void* data)
{
	SSIPeripheralClass* peripheral = SSI_PERIPHERAL_CLASS(klass);
	peripheral->realize = realize;
	peripheral->transfer = transfer;
	peripheral->set_cs = selectChanged;
	peripheral->cs_polarity = SSI_CS_LOW;
	
	DeviceClass* device = DEVICE_CLASS(klass);
	device_class_set_props(device, Properties);
}

static const TypeInfo DeviceTypes[] = {
	{
		.name = TYPE_CO5300,
		.parent = TYPE_SSI_PERIPHERAL,
		.instance_size = sizeof(Co5300),
		.instance_init = instanceInit,
		.instance_finalize = instanceFinalize,
		.class_init = classInit,
	},
};

DEFINE_TYPES(DeviceTypes)
