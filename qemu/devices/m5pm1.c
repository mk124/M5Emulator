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

/* M5PM1: fixed power readings, GPIO IRQ aggregation and power-button events. */

#include "qemu/osdep.h"
#include "m5pm1.h"

#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"

enum
{
	PinCount = 5,
	
	ButtonPressed = 1,
	ButtonPressFlag = 0x80,
	SingleClickIrq = 1,
	DoubleClickIrq = 4,
};

enum
{
	PowerControlReg = 0x06,
	DirectionReg = 0x10,
	OutputReg = 0x11,
	InputReg = 0x12,
	GpioFunctionReg = 0x16,
	GpioIrqStatusReg = 0x40,
	IrqStatus1Reg = 0x41,
	ButtonIrqStatusReg = 0x42,
	GpioIrqMaskReg = 0x43,
	IrqMask1Reg = 0x44,
	ButtonIrqMaskReg = 0x45,
	ButtonStatusReg = 0x48,
	ButtonClickReg = 0x49,
	ButtonActionReg = 0x4A,
};

OBJECT_DECLARE_SIMPLE_TYPE(M5pm1, M5PM1)

struct M5pm1
{
	I2CSlave parentObj;
	uint8_t registers[256], registerAddress;
	bool registerPhase;
	
	uint8_t inputs;
	qemu_irq gpio[PinCount];
	
	QEMUTimer* clickTimer;
	int64_t pressedAtNs, releasedAtNs;
	bool secondClick;
};

static unsigned gpioFunction(const M5pm1* device, unsigned pin)
{
	return (device->registers[GpioFunctionReg + pin / 4] >> ((pin % 4) * 2)) & 3;
}

static void updateOutputs(M5pm1* device)
{
	uint8_t* registers = device->registers;
	bool hasIrqOutput = false;
	for (unsigned pin = 0; pin < PinCount; pin++) hasIrqOutput |= gpioFunction(device, pin) == 1;
	if (!hasIrqOutput) memset(registers + GpioIrqStatusReg, 0, 3);
	
	const bool pending = (registers[GpioIrqStatusReg] & ~registers[GpioIrqMaskReg]) ||
	                     (registers[IrqStatus1Reg] & ~registers[IrqMask1Reg]) ||
	                     (registers[ButtonIrqStatusReg] & ~registers[ButtonIrqMaskReg]);
	uint8_t levels = device->inputs;
	for (unsigned pin = 0; pin < PinCount; pin++)
	{
		const unsigned mask = 1U << pin;
		if (gpioFunction(device, pin) == 1)
		{
			levels = (levels & ~mask) | (pending ? 0 : mask);
		}
		else if (registers[DirectionReg] & mask)
		{
			levels = (levels & ~mask) | (registers[OutputReg] & mask);
		}
		qemu_set_irq(device->gpio[pin], (levels & mask) != 0);
	}
	registers[InputReg] = levels;
}

static void gpioInput(void* opaque, int pin, int level)
{
	M5pm1* device = opaque;
	const uint8_t mask = 1U << pin;
	const bool changed = ((device->inputs & mask) != 0) != (level != 0);
	if (!changed) return;
	
	device->inputs = (device->inputs & ~mask) | (level ? mask : 0);
	if (!(device->registers[DirectionReg] & mask) && gpioFunction(device, pin) == 0)
	{
		device->registers[GpioIrqStatusReg] |= mask;
	}
	updateOutputs(device);
}

static void clickExpired(void* opaque)
{
	M5pm1* device = opaque;
	if (device->registers[ButtonClickReg] & 1)
	{
		device->registers[ButtonIrqStatusReg] |= SingleClickIrq;
		updateOutputs(device);
	}
	else
	{
		qemu_log_mask(LOG_UNIMP, "m5pm1: single-click reset unsupported\n");
	}
}

static void buttonInput(void* opaque, int line, int level)
{
	M5pm1* device = opaque;
	uint8_t* status = &device->registers[ButtonStatusReg];
	const bool pressed = level != 0;
	if (pressed == ((*status & ButtonPressed) != 0)) return;
	
	/* BTN_FLAG latches only a new press; reads clear it even while held. */
	if (pressed) *status |= ButtonPressFlag;
	*status = (*status & ~ButtonPressed) | pressed;
	
	const int64_t nowNs = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	const uint8_t config = device->registers[ButtonClickReg];
	if (pressed)
	{
		const int64_t doubleDelayNs = 125000000LL << ((config >> 5) & 3);
		device->secondClick = timer_pending(device->clickTimer) && nowNs - device->releasedAtNs <= doubleDelayNs;
		if (timer_pending(device->clickTimer))
		{
			timer_del(device->clickTimer);
			/* A separate press commits the previous click instead of losing it. */
			if (!device->secondClick) clickExpired(device);
		}
		device->pressedAtNs = nowNs;
		return;
	}
	
	const int64_t longDelayNs = (1 + ((config >> 3) & 3)) * 1000000000LL;
	if (nowNs - device->pressedAtNs >= longDelayNs)
	{
		device->secondClick = false;
		return;
	}
	if (device->secondClick)
	{
		device->secondClick = false;
		if (device->registers[ButtonActionReg] & 1)
		{
			device->registers[ButtonIrqStatusReg] |= DoubleClickIrq;
			updateOutputs(device);
		}
		else qemu_log_mask(LOG_UNIMP, "m5pm1: double-click shutdown unsupported\n");
		return;
	}
	
	device->releasedAtNs = nowNs;
	/* Functional click delay; switch bounce and PMIC scan phase are omitted. */
	const int64_t singleDelayNs = 125000000LL << ((config >> 1) & 3);
	const int64_t doubleDelayNs = 125000000LL << ((config >> 5) & 3);
	timer_mod(device->clickTimer, nowNs + MAX(singleDelayNs, doubleDelayNs));
}

static int deviceEvent(I2CSlave* slave, enum i2c_event event)
{
	M5pm1* device = M5PM1(slave);
	if (event == I2C_START_SEND) device->registerPhase = true;
	
	return 0;
}

static uint8_t deviceReceive(I2CSlave* slave)
{
	M5pm1* device = M5PM1(slave);
	uint8_t* registers = device->registers;
	const uint8_t address = device->registerAddress++;
	const uint8_t value = registers[address];
	
	if (address == ButtonStatusReg)
	{
		registers[address] &= ~ButtonPressFlag; /* BTN_FLAG clears on read; BTN_STATE remains. */
	}
	
	return value;
}

static int deviceSend(I2CSlave* slave, uint8_t value)
{
	M5pm1* device = M5PM1(slave);
	if (device->registerPhase)
	{
		device->registerAddress = value;
		device->registerPhase = false;
		return 0;
	}
	
	uint8_t* registers = device->registers;
	const uint8_t address = device->registerAddress++;
	
	if (address >= GpioIrqStatusReg && address <= ButtonIrqStatusReg)
	{
		registers[address] &= value; /* PMIC IRQs are W0C, not W1C. */
	}
	else if ((address >= PowerControlReg && address <= 0x0A) ||
	         (address >= DirectionReg && address <= 0x19 && address != InputReg) ||
	         address == 0x2A || (address >= 0x30 && address <= 0x35) ||
	         (address >= 0x38 && address <= 0x3C) || (address >= GpioIrqMaskReg && address <= ButtonIrqMaskReg) ||
	         address == ButtonClickReg || address == ButtonActionReg || address == 0x50 || address == 0x51 ||
	         (address >= 0x60 && address <= 0xBF))
	{
		registers[address] = value;
		if (address == 0x2A)
		{
			registers[address] &= ~1;
		}
		else if (address == 0x50)
		{
			registers[address] &= ~0x40;
		}
		else if (address == 0x51)
		{
			registers[address] &= ~0x80;
		}
	}
	else if (address == 0x0C && value != 0)
	{
		qemu_log_mask(LOG_UNIMP, "m5pm1: power command 0x%02x unsupported\n", value);
	}
	if ((address >= DirectionReg && address <= 0x17) || (address >= GpioIrqStatusReg && address <= ButtonIrqMaskReg)) updateOutputs(device);
	
	return 0;
}

bool m5pm1LedEnabled(I2CSlave* slave)
{
	return (M5PM1(slave)->registers[PowerControlReg] & 0x10) != 0;
}

static void instanceInit(Object* obj)
{
	M5pm1* device = M5PM1(obj);
	uint8_t* registers = device->registers;
	
	/* M5PM1 product ID; unit UID and firmware revision are synthetic. */
	stw_le_p(registers, 0x2050);
	registers[2] = 1;
	registers[3] = 'W';
	registers[4] = 5; /* Battery and USB present. */
	registers[PowerControlReg] = 0x17; /* Charging, L1/L2 rails and status LED enabled. */
	stw_le_p(registers + 0x20, 3300);
	stw_le_p(registers + 0x22, 3750);
	stw_le_p(registers + 0x24, 5000);
	registers[ButtonClickReg] = 0x2A;
	
	device->inputs = 0x17; /* External IRQs inactive; charger STAT high (not charging). */
	device->clickTimer = timer_new_ns(QEMU_CLOCK_VIRTUAL, clickExpired, device);
	qdev_init_gpio_in_named(DEVICE(obj), buttonInput, "button", 1);
	qdev_init_gpio_in_named(DEVICE(obj), gpioInput, "gpio-in", PinCount);
	qdev_init_gpio_out_named(DEVICE(obj), device->gpio, "gpio", PinCount);
	updateOutputs(device);
}

static void instanceFinalize(Object* obj)
{
	M5pm1* device = M5PM1(obj);
	timer_free(device->clickTimer);
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
		.name = TYPE_M5PM1,
		.parent = TYPE_I2C_SLAVE,
		.instance_size = sizeof(M5pm1),
		.instance_init = instanceInit,
		.instance_finalize = instanceFinalize,
		.class_init = classInit,
	},
};

DEFINE_TYPES(DeviceTypes)
