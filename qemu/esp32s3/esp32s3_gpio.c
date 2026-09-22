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

/* Digital GPIO drive, pull resistors and interrupts. Peripheral output levels can be routed
 * through the GPIO matrix; analog voltage and electrical contention are not modeled. */

#include "qemu/osdep.h"
#include "esp32s3_gpio.h"

#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qemu/module.h"

enum
{
	OutputReg = 0x04,
	OutputEnableReg = 0x20,
	PinConfigReg = 0x74,
	MatrixOutputReg = 0x554,
	
	PinOpenDrain = 1 << 2,
	PinIrqEnableMask = 0x1F << 13,
	IrqRising = 1,
	IrqFalling = 2,
	IrqAnyEdge = 3,
	IrqLowLevel = 4,
	IrqHighLevel = 5,
	
	MatrixSignalMask = 0x1FF,
	MatrixSoftwareOutput = 256,
	MatrixInvertOutput = 1 << 9,
	MatrixSoftwareEnable = 1 << 10,
	MatrixInvertEnable = 1 << 11,
};

OBJECT_DECLARE_SIMPLE_TYPE(Esp32s3Gpio, ESP32S3_GPIO_CONTROLLER)

struct Esp32s3Gpio
{
	SysBusDevice parentObj;
	MemoryRegion mmio;
	uint32_t registers[0x1000 / 4];
	uint32_t strapMode;
	
	uint64_t levels, status;
	uint64_t externalLevels, externalDriven, externalPullups;
	uint64_t pullups, pulldowns;
	int8_t driveLevel[Esp32s3GpioPinCount];
	int8_t peripheralDrive[Esp32s3GpioMatrixSignalCount];
	qemu_irq irq;
	qemu_irq output[Esp32s3GpioPinCount];
	qemu_irq drive[Esp32s3GpioPinCount];
	qemu_irq pad[Esp32s3GpioPinCount];
};

static void gpioIrqUpdate(Esp32s3Gpio* gpio)
{
	uint64_t pending = 0;
	for (unsigned pin = 0; pin < Esp32s3GpioPinCount; pin++)
	{
		if (gpio->registers[(PinConfigReg + pin * 4) / 4] & PinIrqEnableMask)
		{
			pending |= gpio->status & (1ULL << pin);
		}
	}
	qemu_set_irq(gpio->irq, pending != 0);
}

static void setLevel(Esp32s3Gpio* gpio, unsigned pin, bool value)
{
	const bool previousLevel = (gpio->levels >> pin) & 1;
	const unsigned interruptType = (gpio->registers[(PinConfigReg + pin * 4) / 4] >> 7) & 7;
	if (previousLevel == value && interruptType != IrqLowLevel && interruptType != IrqHighLevel) return;
	
	gpio->levels = (gpio->levels & ~(1ULL << pin)) | ((uint64_t) value << pin);
	if ((interruptType == IrqRising && !previousLevel && value) ||
	    (interruptType == IrqFalling && previousLevel && !value) ||
	    (interruptType == IrqAnyEdge && previousLevel != value) ||
	    (interruptType == IrqLowLevel && !value) || (interruptType == IrqHighLevel && value))
	{
		gpio->status |= 1ULL << pin;
	}
	gpioIrqUpdate(gpio);
	qemu_set_irq(gpio->pad[pin], value);
	qemu_set_irq(gpio->output[pin], value);
}

/* A released drive is -1. Separate drive and resolved pad signals prevent
 * an I2C slave's ACK from being fed back as a new master-driven START. */
static void updatePin(Esp32s3Gpio* gpio, unsigned pin)
{
	const uint64_t mask = 1ULL << pin;
	const unsigned bank = pin / 32;
	const uint32_t bit = 1U << (pin % 32);
	
	const uint32_t matrix = gpio->registers[(MatrixOutputReg + pin * 4) / 4];
	const unsigned signal = matrix & MatrixSignalMask;
	const bool software = signal == MatrixSoftwareOutput;
	const int peripheral = signal < Esp32s3GpioMatrixSignalCount ? gpio->peripheralDrive[signal] : -1;
	
	const bool gpioEnabled = (gpio->registers[(OutputEnableReg + bank * 12) / 4] & bit) != 0;
	const bool enabled = ((software || (matrix & MatrixSoftwareEnable)) ? gpioEnabled : peripheral >= 0) ^
	                     !!(matrix & MatrixInvertEnable);
	const bool output = (software ? (gpio->registers[(OutputReg + bank * 12) / 4] & bit) != 0 : peripheral > 0) ^
	                    !!(matrix & MatrixInvertOutput);
	const bool openDrain = (gpio->registers[(PinConfigReg + pin * 4) / 4] & PinOpenDrain) != 0;
	const int drive = enabled && (software || peripheral >= 0) && !(openDrain && output) ? output : -1;
	if (gpio->driveLevel[pin] != drive)
	{
		gpio->driveLevel[pin] = drive;
		qemu_set_irq(gpio->drive[pin], drive);
	}
	
	bool level;
	if (drive >= 0) level = drive != 0;
	else if (gpio->externalDriven & mask) level = (gpio->externalLevels & mask) != 0;
	else if ((gpio->externalPullups | gpio->pullups) & mask) level = true;
	else level = !(gpio->pulldowns & mask); /* Unconnected floating pads resolve high. */
	setLevel(gpio, pin, level);
}

static void updateOutputs(Esp32s3Gpio* gpio)
{
	for (unsigned pin = 0; pin < Esp32s3GpioPinCount; pin++) updatePin(gpio, pin);
}

static void gpioInput(void* opaque, int pin, int level)
{
	Esp32s3Gpio* gpio = opaque;
	const uint64_t mask = 1ULL << pin;
	if (level < 0) gpio->externalDriven &= ~mask;
	else gpio->externalDriven |= mask;
	gpio->externalLevels = (gpio->externalLevels & ~mask) | (level > 0 ? mask : 0);
	updatePin(gpio, pin);
}

static void peripheralInput(void* opaque, int signal, int level)
{
	Esp32s3Gpio* gpio = opaque;
	gpio->peripheralDrive[signal] = level;
	for (unsigned pin = 0; pin < Esp32s3GpioPinCount; pin++)
	{
		if ((gpio->registers[(MatrixOutputReg + pin * 4) / 4] & MatrixSignalMask) == signal) updatePin(gpio, pin);
	}
}

static void pullInput(void* opaque, int pin, int value)
{
	Esp32s3Gpio* gpio = opaque;
	const uint64_t mask = 1ULL << pin;
	gpio->pulldowns = (gpio->pulldowns & ~mask) | ((value & 1) ? mask : 0);
	gpio->pullups = (gpio->pullups & ~mask) | ((value & 2) ? mask : 0);
	updatePin(gpio, pin);
}

static uint64_t gpioRead(void* opaque, hwaddr address, unsigned size)
{
	const Esp32s3Gpio* gpio = opaque;
	
	switch (address)
	{
		case 0x38: return gpio->strapMode;
		case 0x3C: return (uint32_t) gpio->levels;
		case 0x40: return gpio->levels >> 32;
		case 0x44:
		case 0x5C:
		case 0x60:
		case 0x64: return (uint32_t) gpio->status;
		case 0x50:
		case 0x68:
		case 0x6C:
		case 0x70: return gpio->status >> 32;
		default:
			return gpio->registers[address / 4];
	}
}

static void gpioWrite(void* opaque, hwaddr address, uint64_t value, unsigned size)
{
	Esp32s3Gpio* gpio = opaque;
	
	switch (address)
	{
		case 0x08: {
			gpio->registers[OutputReg / 4] |= value;
			break;
		}
		case 0x0C: {
			gpio->registers[OutputReg / 4] &= ~value;
			break;
		}
		case 0x14: {
			gpio->registers[0x10 / 4] |= value;
			break;
		}
		case 0x18: {
			gpio->registers[0x10 / 4] &= ~value;
			break;
		}
		case 0x24:
		case 0x30: {
			gpio->registers[(address - 4) / 4] |= value;
			break;
		}
		case 0x28:
		case 0x34: {
			gpio->registers[(address - 8) / 4] &= ~value;
			break;
		}
		case 0x44: {
			gpio->status = (gpio->status & 0xFFFFFFFF00000000ULL) | value;
			break;
		}
		case 0x48: {
			gpio->status |= value;
			break;
		}
		case 0x4C: {
			gpio->status &= ~value;
			break;
		}
		case 0x50: {
			gpio->status = (uint32_t) gpio->status | (value << 32);
			break;
		}
		case 0x54: {
			gpio->status |= value << 32;
			break;
		}
		case 0x58: {
			gpio->status &= ~(value << 32);
			break;
		}
		default: {
			gpio->registers[address / 4] = value;
			break;
		}
	}
	
	if (address >= OutputReg && address <= 0x34) updateOutputs(gpio);
	else if (address >= PinConfigReg && address < PinConfigReg + Esp32s3GpioPinCount * 4) updatePin(gpio, (address - PinConfigReg) / 4);
	else if (address >= MatrixOutputReg && address < MatrixOutputReg + Esp32s3GpioPinCount * 4) updatePin(gpio, (address - MatrixOutputReg) / 4);
	gpioIrqUpdate(gpio);
}

static void gpioReset(DeviceState* dev)
{
	Esp32s3Gpio* gpio = ESP32S3_GPIO_CONTROLLER(dev);
	memset(gpio->registers, 0, sizeof(gpio->registers));
	gpio->status = 0;
	for (unsigned pin = 0; pin < Esp32s3GpioPinCount; pin++) gpio->registers[(MatrixOutputReg + pin * 4) / 4] = MatrixSoftwareOutput;
	
	updateOutputs(gpio);
	gpioIrqUpdate(gpio);
}

static const MemoryRegionOps GpioOps = {
	.read = gpioRead,
	.write = gpioWrite,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void instanceInit(Object* obj)
{
	Esp32s3Gpio* gpio = ESP32S3_GPIO_CONTROLLER(obj);
	gpio->levels = (1ULL << Esp32s3GpioPinCount) - 1;
	memset(gpio->driveLevel, -1, sizeof(gpio->driveLevel));
	memset(gpio->peripheralDrive, -1, sizeof(gpio->peripheralDrive));
	memory_region_init_io(&gpio->mmio, obj, &GpioOps, gpio, TYPE_ESP32S3_GPIO_CONTROLLER, 0x1000);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &gpio->mmio);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &gpio->irq);
	qdev_init_gpio_in_named(DEVICE(obj), gpioInput, "in", Esp32s3GpioPinCount);
	qdev_init_gpio_in_named(DEVICE(obj), pullInput, "pull", Esp32s3GpioPinCount);
	qdev_init_gpio_in_named(DEVICE(obj), peripheralInput, "matrix", Esp32s3GpioMatrixSignalCount);
	qdev_init_gpio_out_named(DEVICE(obj), gpio->drive, "drive", Esp32s3GpioPinCount);
	qdev_init_gpio_out_named(DEVICE(obj), gpio->output, "out", Esp32s3GpioPinCount);
	qdev_init_gpio_out_named(DEVICE(obj), gpio->pad, "pad", Esp32s3GpioPinCount);
	gpioReset(DEVICE(obj));
}

static Property Properties[] = {
	DEFINE_PROP_UINT32("strap-mode", Esp32s3Gpio, strapMode, 0),
	DEFINE_PROP_UINT64("external-pullups", Esp32s3Gpio, externalPullups, 0),
	DEFINE_PROP_END_OF_LIST(),
};

static void classInit(ObjectClass* klass, void* data)
{
	DeviceClass* device = DEVICE_CLASS(klass);
	device_class_set_props(device, Properties);
	device_class_set_legacy_reset(device, gpioReset);
}

static const TypeInfo DeviceTypes[] = {
	{
		.name = TYPE_ESP32S3_GPIO_CONTROLLER,
		.parent = TYPE_SYS_BUS_DEVICE,
		.instance_size = sizeof(Esp32s3Gpio),
		.instance_init = instanceInit,
		.class_init = classInit,
	},
};

DEFINE_TYPES(DeviceTypes)
