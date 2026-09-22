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

/* RX8130CE: calendar, STOP and minute-carry alarms; no timer/update IRQs. */

#include "qemu/osdep.h"
#include "rx8130ce.h"

#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "sysemu/rtc.h"
#include "sysemu/sysemu.h"

enum
{
	AlarmMask = 0x80,
	DayAlarm = 0x08,
	AlarmFlag = 0x08,
	VoltageLowFlag = 0x02,
	AlarmEnable = 0x08,
	Stop = 0x40
};

enum
{
	SecondsReg = 0x10,
	YearReg = 0x16,
	MinuteAlarmReg = 0x17,
	HourAlarmReg = 0x18,
	DayWeekAlarmReg = 0x19,
	ExtensionReg = 0x1C,
	FlagReg = 0x1D,
	ControlReg = 0x1E,
};

OBJECT_DECLARE_SIMPLE_TYPE(Rx8130ce, RX8130CE)

struct Rx8130ce
{
	I2CSlave parentObj;
	uint8_t registers[256], registerAddress;
	bool registerPhase;
	
	int64_t baseTimeNs, baseClockNs;
	uint8_t week;
	bool dirty, secondsWritten;
	QEMUTimer* alarmTimer;
	qemu_irq irq;
};

static uint8_t toBcd(unsigned value)
{
	return (value / 10 << 4) | (value % 10);
}

static unsigned fromBcd(uint8_t value)
{
	return (value >> 4) * 10 + (value & 15);
}

static int64_t calendarTimeNs(const Rx8130ce* device)
{
	if (device->registers[ControlReg] & Stop) return device->baseTimeNs;
	return device->baseTimeNs + qemu_clock_get_ns(rtc_clock) - device->baseClockNs;
}

static uint8_t weekAt(const Rx8130ce* device, time_t seconds)
{
	/* WEEK rotates at midnight independently of the programmed calendar date. */
	const int days = (seconds / 86400 - device->baseTimeNs / NANOSECONDS_PER_SECOND / 86400) % 7;
	const unsigned shift = (days + 7) % 7;
	return ((device->week << shift) | (device->week >> (7 - shift))) & 0x7F;
}

static void updateAlarm(Rx8130ce* device)
{
	const uint8_t* registers = device->registers;
	qemu_set_irq(device->irq, !((registers[FlagReg] & AlarmFlag) && (registers[ControlReg] & AlarmEnable)));
	timer_del(device->alarmTimer);
	if ((registers[ControlReg] & Stop) || (registers[FlagReg] & AlarmFlag)) return;
	
	const unsigned minute = fromBcd(registers[MinuteAlarmReg] & 0x7F);
	const unsigned hour = fromBcd(registers[HourAlarmReg] & 0x3F);
	const unsigned day = fromBcd(registers[DayWeekAlarmReg] & 0x3F);
	if (!(registers[MinuteAlarmReg] & AlarmMask) && (minute > 59 || (registers[MinuteAlarmReg] & 15) > 9)) return;
	if (!(registers[HourAlarmReg] & AlarmMask) && (hour > 23 || (registers[HourAlarmReg] & 15) > 9)) return;
	if (!(registers[DayWeekAlarmReg] & AlarmMask))
	{
		if (registers[ExtensionReg] & DayAlarm)
		{
			if (!day || day > 31 || (registers[DayWeekAlarmReg] & 15) > 9) return;
		}
		else if (!(registers[DayWeekAlarmReg] & 0x7F) || !device->week) return;
	}
	
	/* Epson ETM50E-10, 14.3: compare on internal minute carries, even with AIE=0.
	 * Schedule the next match directly so a late host callback cannot miss it. */
	time_t next = calendarTimeNs(device) / NANOSECONDS_PER_SECOND;
	next += 60 - next % 60;
	for (;;)
	{
		struct tm calendar;
		gmtime_r(&next, &calendar);
		if (!(registers[DayWeekAlarmReg] & AlarmMask) &&
		    ((registers[ExtensionReg] & DayAlarm) ? day != calendar.tm_mday : !(registers[DayWeekAlarmReg] & weekAt(device, next))))
		{
			next += (24 * 60 - calendar.tm_hour * 60 - calendar.tm_min) * 60;
			continue;
		}
		if (!(registers[HourAlarmReg] & AlarmMask) && hour != calendar.tm_hour)
		{
			next += ((hour + 24 - calendar.tm_hour) % 24) * 3600 - calendar.tm_min * 60;
			continue;
		}
		if (!(registers[MinuteAlarmReg] & AlarmMask) && minute != calendar.tm_min)
		{
			next += ((minute + 60 - calendar.tm_min) % 60) * 60;
			continue;
		}
		break;
	}
	timer_mod_ns(device->alarmTimer, device->baseClockNs + (int64_t) next * NANOSECONDS_PER_SECOND - device->baseTimeNs);
}

static void alarmExpired(void* opaque)
{
	Rx8130ce* device = opaque;
	device->registers[FlagReg] |= AlarmFlag;
	updateAlarm(device);
}

static void snapshot(Rx8130ce* device)
{
	uint8_t* registers = device->registers;
	if (device->dirty || (registers[ControlReg] & Stop)) return;
	
	const time_t seconds = calendarTimeNs(device) / NANOSECONDS_PER_SECOND;
	struct tm calendar;
	gmtime_r(&seconds, &calendar);
	registers[SecondsReg] = toBcd(calendar.tm_sec);
	registers[0x11] = toBcd(calendar.tm_min);
	registers[0x12] = toBcd(calendar.tm_hour);
	registers[0x13] = weekAt(device, seconds);
	registers[0x14] = toBcd(calendar.tm_mday);
	registers[0x15] = toBcd(calendar.tm_mon + 1);
	registers[YearReg] = toBcd((calendar.tm_year + 1900) % 100);
}

static void commit(Rx8130ce* device)
{
	uint8_t* registers = device->registers;
	if (!device->dirty || (registers[ControlReg] & Stop)) return;
	
	struct tm calendar = {
		.tm_sec = fromBcd(registers[SecondsReg] & 0x7F),
		.tm_min = fromBcd(registers[0x11] & 0x7F),
		.tm_hour = fromBcd(registers[0x12] & 0x3F),
		.tm_mday = fromBcd(registers[0x14] & 0x3F),
		.tm_mon = (int) fromBcd(registers[0x15] & 0x1F) - 1,
		.tm_year = 100 + fromBcd(registers[YearReg]),
	};
	
	const int64_t fractionNs = device->secondsWritten ? 0 : calendarTimeNs(device) % NANOSECONDS_PER_SECOND;
	device->dirty = device->secondsWritten = false;
	if (calendar.tm_sec > 59 || calendar.tm_min > 59 || calendar.tm_hour > 23 ||
	    !g_date_valid_dmy(calendar.tm_mday, calendar.tm_mon + 1, calendar.tm_year + 1900))
	{
		registers[FlagReg] |= VoltageLowFlag; /* Invalid calendar: expose VLF, never invent a date. */
		return;
	}
	
	device->baseTimeNs = (int64_t) mktimegm(&calendar) * NANOSECONDS_PER_SECOND + fractionNs;
	device->baseClockNs = qemu_clock_get_ns(rtc_clock);
	device->week = registers[0x13] & 0x7F;
	updateAlarm(device);
}

static int deviceEvent(I2CSlave* slave, enum i2c_event event)
{
	Rx8130ce* device = RX8130CE(slave);
	if (event == I2C_START_SEND || event == I2C_START_RECV)
	{
		commit(device);
		snapshot(device);
	}
	else if (event == I2C_FINISH) commit(device);
	if (event == I2C_START_SEND) device->registerPhase = true;
	
	return 0;
}

static uint8_t deviceReceive(I2CSlave* slave)
{
	Rx8130ce* device = RX8130CE(slave);
	return device->registers[device->registerAddress++];
}

static int deviceSend(I2CSlave* slave, uint8_t value)
{
	Rx8130ce* device = RX8130CE(slave);
	if (device->registerPhase)
	{
		device->registerAddress = value;
		device->registerPhase = false;
		return 0;
	}
	
	uint8_t* registers = device->registers;
	const uint8_t address = device->registerAddress++;
	
	if (address == FlagReg)
	{
		registers[address] &= value; /* Alarm/timer/update/voltage flags are W0C. */
		updateAlarm(device);
	}
	else if (address == ControlReg)
	{
		commit(device);
		snapshot(device);
		if ((registers[address] ^ value) & Stop)
		{
			const int64_t timeNs = calendarTimeNs(device);
			device->week = weekAt(device, timeNs / NANOSECONDS_PER_SECOND);
			device->baseTimeNs = timeNs;
			device->baseClockNs = qemu_clock_get_ns(rtc_clock);
		}
		registers[address] = value;
		commit(device);
		updateAlarm(device);
	}
	else if (address >= SecondsReg && address <= 0x23)
	{
		registers[address] = value;
		device->dirty |= address <= YearReg;
		device->secondsWritten |= address == SecondsReg;
		if ((address >= MinuteAlarmReg && address <= DayWeekAlarmReg) || address == ExtensionReg) updateAlarm(device);
	}
	
	return 0;
}

static void instanceInit(Object* obj)
{
	Rx8130ce* device = RX8130CE(obj);
	struct tm calendar;
	qemu_get_timedate(&calendar, 0);
	device->baseClockNs = qemu_clock_get_ns(rtc_clock);
	device->baseTimeNs = (int64_t) mktimegm(&calendar) * NANOSECONDS_PER_SECOND + device->baseClockNs % NANOSECONDS_PER_SECOND;
	device->week = 1U << calendar.tm_wday;
	
	device->alarmTimer = timer_new_ns(rtc_clock, alarmExpired, device);
	qdev_init_gpio_out_named(DEVICE(obj), &device->irq, "irq", 1);
	snapshot(device);
	updateAlarm(device);
}

static void instanceFinalize(Object* obj)
{
	timer_free(RX8130CE(obj)->alarmTimer);
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
		.name = TYPE_RX8130CE,
		.parent = TYPE_I2C_SLAVE,
		.instance_size = sizeof(Rx8130ce),
		.instance_init = instanceInit,
		.instance_finalize = instanceFinalize,
		.class_init = classInit,
	},
};

DEFINE_TYPES(DeviceTypes)
