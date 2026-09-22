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

/* CST820: single-point touch, state-change IRQ pulses and power/reset recovery. */

#include "qemu/osdep.h"
#include "cst820.h"

#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"

enum
{
	StartupDelayNs = 100000000,
	IrqPulseUnitNs = 100000,
	ChangeInterrupt = 0x20,
};

enum
{
	TouchCountReg = 0x02,
	TouchXHighReg = 0x03,
	TouchXLowReg = 0x04,
	TouchYHighReg = 0x05,
	TouchYLowReg = 0x06,
	SleepModeReg = 0xE5,
	IrqPulseWidthReg = 0xED,
	IrqControlReg = 0xFA,
	AutoSleepReg = 0xFE,
};

OBJECT_DECLARE_SIMPLE_TYPE(Cst820, CST820)

struct Cst820
{
	I2CSlave parentObj;
	uint8_t registers[256], registerAddress;
	bool registerPhase;
	
	uint16_t x, y;
	bool down, powered, resetAsserted;
	int64_t touchReadyAtNs;
	
	qemu_irq irq;
	QEMUTimer* irqTimer;
};

static bool isActive(const Cst820* device)
{
	return device->powered && !device->resetAsserted;
}

static void releaseIrq(void* opaque)
{
	Cst820* device = opaque;
	qemu_set_irq(device->irq, 1);
}

static void updateTouch(Cst820* device)
{
	uint8_t* registers = device->registers;
	const int64_t nowNs = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	const bool ready = isActive(device) && registers[SleepModeReg] != 3 && nowNs >= device->touchReadyAtNs;
	const bool down = ready && device->down;
	const uint16_t previousX = ((registers[TouchXHighReg] & 0x0F) << 8) | registers[TouchXLowReg];
	const uint16_t previousY = ((registers[TouchYHighReg] & 0x0F) << 8) | registers[TouchYLowReg];
	const bool changed = down != (registers[TouchCountReg] != 0) || (down && (device->x != previousX || device->y != previousY));
	
	registers[TouchCountReg] = down;
	registers[TouchXHighReg] = (down ? 0x80 : 0x40) | (device->x >> 8);
	registers[TouchXLowReg] = device->x;
	registers[TouchYHighReg] = device->y >> 8;
	registers[TouchYLowReg] = device->y;
	
	if (!ready || !(registers[IrqControlReg] & ChangeInterrupt))
	{
		timer_del(device->irqTimer);
		qemu_set_irq(device->irq, 1);
		return;
	}
	
	/* Coalesce changes during a pulse into the latest report without extending
	 * the low level. I2C reads and unchanged input never retrigger an IRQ. */
	if (changed && !timer_pending(device->irqTimer))
	{
		qemu_set_irq(device->irq, 0);
		timer_mod(device->irqTimer, nowNs + registers[IrqPulseWidthReg] * IrqPulseUnitNs);
	}
}

static void resetRegisters(Cst820* device)
{
	memset(device->registers, 0, sizeof(device->registers));
	device->registers[0xA7] = 0xB5;
	device->registers[0xA9] = 1; /* Model firmware revision. */
	device->registers[IrqPulseWidthReg] = 10; /* 1 ms; IRQ modes remain disabled until configured. */
	device->registerAddress = 0;
	device->registerPhase = false;
	updateTouch(device);
}

static void powerInput(void* opaque, int line, int level)
{
	Cst820* device = opaque;
	const bool powered = level != 0;
	if (device->powered == powered) return;
	device->powered = powered;
	device->touchReadyAtNs = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + StartupDelayNs;
	resetRegisters(device);
}

static void resetInput(void* opaque, int line, int level)
{
	Cst820* device = opaque;
	const bool asserted = level == 0;
	if (device->resetAsserted == asserted) return;
	device->resetAsserted = asserted;
	
	if (asserted) resetRegisters(device);
	else
	{
		device->touchReadyAtNs = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + StartupDelayNs;
		updateTouch(device);
	}
}

static int deviceEvent(I2CSlave* slave, enum i2c_event event)
{
	Cst820* device = CST820(slave);
	if (event == I2C_START_SEND || event == I2C_START_RECV)
	{
		/* Allow register access while touch scanning initializes. The datasheet
		 * specifies full initialization time, not the first I2C ACK deadline. */
		if (!isActive(device)) return 1;
		updateTouch(device);
	}
	if (event == I2C_START_SEND) device->registerPhase = true;
	
	return 0;
}

static uint8_t deviceReceive(I2CSlave* slave)
{
	Cst820* device = CST820(slave);
	if (!isActive(device)) return 0xFF;
	return device->registers[device->registerAddress++];
}

static int deviceSend(I2CSlave* slave, uint8_t value)
{
	Cst820* device = CST820(slave);
	if (!isActive(device)) return 1;
	if (device->registerPhase)
	{
		device->registerAddress = value;
		device->registerPhase = false;
		return 0;
	}
	
	uint8_t* registers = device->registers;
	const uint8_t address = device->registerAddress++;
	
	if (address == IrqPulseWidthReg)
	{
		registers[address] = CLAMP(value, 1, 200);
	}
	else if (address == SleepModeReg || address == IrqControlReg || address == AutoSleepReg)
	{
		registers[address] = value; /* Sleep/interrupt/auto-sleep configuration. */
		if (address == IrqControlReg && (value & 0xD1))
		{
			qemu_log_mask(LOG_UNIMP, "CST820: periodic and gesture IRQ modes (0x%02x) are not modeled\n", value & 0xD1);
		}
		if (address == SleepModeReg || address == IrqControlReg) updateTouch(device);
	}
	
	return 0;
}

void cst820SetTouch(I2CSlave* slave, uint16_t x, uint16_t y, bool down)
{
	Cst820* device = CST820(slave);
	device->x = x;
	device->y = y;
	device->down = down;
	updateTouch(device);
}

static void instanceInit(Object* obj)
{
	Cst820* device = CST820(obj);
	device->resetAsserted = true;
	device->irqTimer = timer_new_ns(QEMU_CLOCK_VIRTUAL, releaseIrq, device);
	qdev_init_gpio_in_named(DEVICE(obj), powerInput, "power", 1);
	qdev_init_gpio_in_named(DEVICE(obj), resetInput, "reset", 1);
	qdev_init_gpio_out_named(DEVICE(obj), &device->irq, "irq", 1);
	resetRegisters(device);
}

static void instanceFinalize(Object* obj)
{
	Cst820* device = CST820(obj);
	timer_free(device->irqTimer);
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
		.name = TYPE_CST820,
		.parent = TYPE_I2C_SLAVE,
		.instance_size = sizeof(Cst820),
		.instance_init = instanceInit,
		.instance_finalize = instanceFinalize,
		.class_init = classInit,
	},
};

DEFINE_TYPES(DeviceTypes)
