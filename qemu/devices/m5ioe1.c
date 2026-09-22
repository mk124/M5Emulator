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

/* M5IOE1: GPIO, mean PWM drive and nominal ADC readings; no PWM edges or NeoPixel output. */

#include "qemu/osdep.h"
#include "m5ioe1.h"

#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "qemu/bswap.h"
#include "qemu/module.h"

enum
{
	PinCount = 14,
	
	PwmEnabled = 0x8000,
	PwmInverted = 0x4000,
	PwmDutyMask = 0x0FFF,
};

enum
{
	DirectionReg = 0x03,
	OutputReg = 0x05,
	InputReg = 0x07,
	PullupReg = 0x09,
	PulldownReg = 0x0B,
	OpenDrainReg = 0x13,
	PwmDutyReg = 0x1B,
	PwmFrequencyReg = 0x25,
	CommandReg = 0x29,
	OneWireReg = 0x90,
};

OBJECT_DECLARE_SIMPLE_TYPE(M5ioe1, M5IOE1)

struct M5ioe1
{
	I2CSlave parentObj;
	uint8_t registers[256], registerAddress;
	bool registerPhase;
	
	qemu_irq gpio[PinCount];
	qemu_irq drive[PinCount]; /* 0/1 driven; -1 released (input or open-drain high). */
};

static void updateOutputs(M5ioe1* device)
{
	const uint16_t direction = lduw_le_p(device->registers + DirectionReg);
	const uint16_t output = lduw_le_p(device->registers + OutputReg);
	const uint16_t openDrain = lduw_le_p(device->registers + OpenDrainReg);
	for (unsigned pin = 0; pin < PinCount; pin++)
	{
		const unsigned mask = 1U << pin;
		const bool high = (output & mask) != 0;
		qemu_set_irq(device->gpio[pin], ((direction & output) >> pin) & 1);
		qemu_set_irq(device->drive[pin], (direction & mask) && !(high && (openDrain & mask)) ? high : -1);
	}
}

static void resetRegisters(M5ioe1* device)
{
	uint8_t* registers = device->registers;
	
	memset(registers, 0, sizeof(device->registers));
	stw_le_p(registers, 1); /* Synthetic unit UID, not a physical device's serial. */
	registers[0x02] = 'W'; /* Driver requires W for the fitted 0x4F address. */
	stw_le_p(registers + 0x27, 3300);
	stw_le_p(registers + PwmFrequencyReg, 500);
	device->registerAddress = 0;
	updateOutputs(device);
}

static int deviceEvent(I2CSlave* slave, enum i2c_event event)
{
	M5ioe1* device = M5IOE1(slave);
	if (event == I2C_START_SEND) device->registerPhase = true;
	
	return 0;
}

static uint8_t deviceReceive(I2CSlave* slave)
{
	M5ioe1* device = M5IOE1(slave);
	uint8_t* registers = device->registers;
	const uint8_t address = device->registerAddress++;
	uint8_t value = registers[address];
	
	if (address == InputReg || address == 0x08)
	{
		const unsigned i = address - InputReg;
		value = (registers[OutputReg + i] & registers[DirectionReg + i]) |
		        (registers[PullupReg + i] & ~registers[PulldownReg + i] & ~registers[DirectionReg + i]);
		if (i) value &= 0x3F;
	}
	
	return value;
}

static int deviceSend(I2CSlave* slave, uint8_t value)
{
	M5ioe1* device = M5IOE1(slave);
	if (device->registerPhase)
	{
		device->registerAddress = value;
		device->registerPhase = false;
		return 0;
	}
	
	uint8_t* registers = device->registers;
	const uint8_t address = device->registerAddress++;
	
	if (address == CommandReg)
	{
		if (value == 0x3A)
		{
			resetRegisters(device);
		}
	}
	else if (address == 0x11 || address == 0x12)
	{
		registers[address] &= value; /* GPIO interrupt status: write zero to clear. */
	}
	else if ((address >= DirectionReg && address <= 0x06) ||
	         (address >= PullupReg && address <= 0x10) ||
	         (address >= OpenDrainReg && address <= 0x15) || address == 0x18 ||
	         (address >= PwmDutyReg && address <= 0x26) || (address >= 0x30 && address <= OneWireReg))
	{
		registers[address] = value;
		if ((address >= DirectionReg && address <= 0x06) || address == OpenDrainReg || address == 0x14) updateOutputs(device);
		if (address == 0x15 || address == 0x18 || address == 0x24)
		{
			/* Nominal unconnected ADC / no temperature model / LED refresh. */
			registers[address] &= ~0xC0;
		}
		else if (address == OneWireReg && (value & 0x80))
		{
			const unsigned pin = value & 0x1F;
			if (pin < PinCount)
			{
				/* One-wire mode selection ends high, including zero extra
				 * pulses (mode 1). Model the latched output, not pulse timing
				 * or the amplifier's analog gain/limiter. */
				const uint16_t mask = 1U << pin;
				stw_le_p(registers + DirectionReg, lduw_le_p(registers + DirectionReg) | mask);
				stw_le_p(registers + OutputReg, lduw_le_p(registers + OutputReg) | mask);
				updateOutputs(device);
			}
			registers[address] &= ~0x80;
		}
	}
	
	return 0;
}

uint16_t m5ioe1OutputDuty(I2CSlave* slave, unsigned pin)
{
	const uint8_t* registers = M5IOE1(slave)->registers;
	if (pin >= PinCount || !(lduw_le_p(registers + DirectionReg) & (1U << pin))) return 0;
	
	static const unsigned PwmPins[] = { 8, 7, 10, 9 };
	for (unsigned channel = 0; channel < ARRAY_SIZE(PwmPins); channel++)
	{
		if (PwmPins[channel] != pin) continue;
		
		const uint16_t config = lduw_le_p(registers + PwmDutyReg + channel * 2);
		if (!(config & PwmEnabled)) break;
		if (!lduw_le_p(registers + PwmFrequencyReg)) return 0;
		
		unsigned duty = config & PwmDutyMask;
		if (config & PwmInverted) duty = 4095 - duty;
		return duty * 65535U / 4095;
	}
	
	return (lduw_le_p(registers + OutputReg) & (1U << pin)) ? 65535 : 0;
}

static void instanceInit(Object* obj)
{
	M5ioe1* device = M5IOE1(obj);
	qdev_init_gpio_out_named(DEVICE(obj), device->gpio, "gpio", PinCount);
	qdev_init_gpio_out_named(DEVICE(obj), device->drive, "drive", PinCount);
	resetRegisters(device);
}

static void classInit(ObjectClass* klass, void* data)
{
	I2CSlaveClass* slave = I2C_SLAVE_CLASS(klass);
	slave->event = deviceEvent;
	slave->recv = deviceReceive;
	slave->send = deviceSend;
}

static const TypeInfo DeviceTypes[] = {
	{
		.name = TYPE_M5IOE1,
		.parent = TYPE_I2C_SLAVE,
		.instance_size = sizeof(M5ioe1),
		.instance_init = instanceInit,
		.class_init = classInit,
	},
};

DEFINE_TYPES(DeviceTypes)
