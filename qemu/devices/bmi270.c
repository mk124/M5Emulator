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

/* BMI270 base configuration: six-axis sampling, data-ready and any-motion IRQs.
 * See BST-BMI270-DS000-08 sections 4.6, 4.8.2, 4.9 and Bosch BMI270_SensorAPI.
 * Configuration upload is checked for completeness; microcode is not executed.
 * FIFO, analog filtering, bandwidth/averaging and sensor startup delays are not modeled. */

#include "qemu/osdep.h"
#include "bmi270.h"

#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#include <math.h>

enum
{
	ConfigSize = 8192,
	FeaturePageCount = 8,
	FeaturePageSize = 16,
	AnyMotionPeriodNs = 20000000,
	DataReadyPulseNs = 156250, /* 1 / 6400 Hz, independent of sensor ODR. */
};

enum
{
	StatusReg = 0x03,
	AccelDataReg = 0x0C,
	GyroDataReg = 0x12,
	FeatureStatusReg = 0x1C,
	DataStatusReg = 0x1D,
	InternalStatusReg = 0x21,
	FeaturePageReg = 0x2F,
	FeatureDataReg = 0x30,
	AccelConfReg = 0x40,
	AccelRangeReg = 0x41,
	GyroConfReg = 0x42,
	GyroRangeReg = 0x43,
	Int1ControlReg = 0x53,
	IntLatchReg = 0x55,
	Int1MapReg = 0x56,
	IntDataMapReg = 0x58,
	InitControlReg = 0x59,
	InitAddressLowReg = 0x5B,
	InitAddressHighReg = 0x5C,
	InitDataReg = 0x5E,
	PowerConfReg = 0x7C,
	PowerControlReg = 0x7D,
	CommandReg = 0x7E,
};

OBJECT_DECLARE_SIMPLE_TYPE(Bmi270, BMI270)

struct Bmi270
{
	I2CSlave parentObj;
	uint8_t registers[256], registerAddress;
	bool registerPhase;
	
	uint8_t config[ConfigSize], written[ConfigSize / 8];
	unsigned configOffset, loadedBytes;
	
	uint8_t features[FeaturePageCount][FeaturePageSize];
	QEMUTimer* sampleTimer;
	int64_t nextSampleNs[2], dataReadyUntilNs[2];
	float accelerationG[3], angularVelocityDps[3];
	
	QEMUTimer* motionTimer;
	float motionReferenceG[3];
	unsigned motionSamples;
	uint8_t motionActive;
	qemu_irq irq[2];
};

static void updateIrqs(Bmi270* device)
{
	const uint8_t* registers = device->registers;
	const int64_t nowNs = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	const bool latched = registers[IntLatchReg] & 1;
	const uint8_t features = latched ? registers[FeatureStatusReg] : device->motionActive;
	const bool dataReady = latched
	                           ? (registers[DataStatusReg] & 0xC0) != 0
	                           : nowNs < device->dataReadyUntilNs[0] || nowNs < device->dataReadyUntilNs[1];
	
	for (unsigned pin = 0; pin < 2; pin++)
	{
		const uint8_t control = registers[Int1ControlReg + pin];
		const bool active = (features & registers[Int1MapReg + pin]) ||
		                    (dataReady && (registers[IntDataMapReg] & (4U << (pin * 4))));
		/* QEMU GPIOs have no high impedance. Disabled outputs resolve low for
		 * the board's Q7 bias; enabled open-drain highs assume an external pull-up. */
		const bool level = (control & 8) && (active == ((control & 2) != 0));
		qemu_set_irq(device->irq[pin], level);
	}
}

static int64_t sensorPeriodNs(const Bmi270* device, unsigned sensor)
{
	const uint8_t* registers = device->registers;
	const unsigned odr = registers[AccelConfReg + sensor * 2] & 15;
	const bool accelerometer = sensor == 0;
	const bool valid = accelerometer ? odr >= 1 && odr <= 12 : odr >= 6 && odr <= 13 && (registers[GyroRangeReg] & 7) <= 4;
	if (!valid || registers[InternalStatusReg] != 1 || !(registers[PowerControlReg] & (accelerometer ? 4 : 2))) return 0;
	
	return 1280000000LL >> (odr - 1);
}

static void sampleSensors(void* opaque)
{
	Bmi270* device = opaque;
	uint8_t* registers = device->registers;
	const int64_t nowNs = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	int64_t nextEventNs = INT64_MAX;
	
	for (unsigned sensor = 0; sensor < 2; sensor++)
	{
		const bool accelerometer = sensor == 0;
		const int64_t periodNs = sensorPeriodNs(device, sensor);
		const unsigned range = registers[AccelRangeReg + sensor * 2] & (accelerometer ? 3 : 7);
		const uint8_t ready = accelerometer ? 0x80 : 0x40;
		
		if (!periodNs)
		{
			registers[StatusReg] &= ~ready;
			registers[DataStatusReg] &= ~ready;
			device->nextSampleNs[sensor] = device->dataReadyUntilNs[sensor] = 0;
			continue;
		}
		if (!device->nextSampleNs[sensor]) device->nextSampleNs[sensor] = nowNs + periodNs;
		if (nowNs >= device->nextSampleNs[sensor])
		{
			registers[StatusReg] |= ready;
			registers[DataStatusReg] |= ready;
			device->nextSampleNs[sensor] = nowNs + periodNs;
			device->dataReadyUntilNs[sensor] = nowNs + DataReadyPulseNs;
			
			const float* input = accelerometer ? device->accelerationG : device->angularVelocityDps;
			const float scale = accelerometer ? (16384U >> range) : 32768.0f / (2000U >> range);
			for (unsigned axis = 0; axis < 3; axis++)
			{
				const int16_t value = lroundf(CLAMP(input[axis] * scale, -32768.0f, 32767.0f));
				stw_le_p(registers + (accelerometer ? AccelDataReg : GyroDataReg) + axis * 2, value);
			}
		}
		nextEventNs = MIN(nextEventNs, device->nextSampleNs[sensor]);
		if (nowNs < device->dataReadyUntilNs[sensor])
		{
			nextEventNs = MIN(nextEventNs, device->dataReadyUntilNs[sensor]);
		}
	}
	
	if (nextEventNs == INT64_MAX) timer_del(device->sampleTimer);
	else timer_mod(device->sampleTimer, nextEventNs);
	updateIrqs(device);
}

static int64_t motionPeriodNs(const Bmi270* device)
{
	const int64_t periodNs = sensorPeriodNs(device, 0);
	const uint16_t axesDuration = lduw_le_p(device->features[1] + 0x0C);
	if (!periodNs || !(axesDuration & 0xE000) || !(device->features[1][0x0F] & 0x80)) return 0;
	
	/* In power-optimized mode feature evaluation slows below the required 50 Hz. */
	return device->registers[AccelConfReg] & 0x80 ? AnyMotionPeriodNs : MAX(AnyMotionPeriodNs, periodNs);
}

static void sampleMotion(void* opaque)
{
	Bmi270* device = opaque;
	const uint16_t axesDuration = lduw_le_p(device->features[1] + 0x0C);
	const uint16_t thresholdOutput = lduw_le_p(device->features[1] + 0x0E);
	const float thresholdG = (thresholdOutput & 0x07FF) / 2048.0f;
	const unsigned output = (thresholdOutput >> 11) & 15;
	const float rangeG = 2U << (device->registers[AccelRangeReg] & 3);
	
	float accelerationG[3];
	bool exceeds = false;
	for (unsigned axis = 0; axis < 3; axis++)
	{
		accelerationG[axis] = CLAMP(device->accelerationG[axis], -rangeG, rangeG);
		if (axesDuration & (0x2000U << axis))
		{
			exceeds |= fabsf(accelerationG[axis] - device->motionReferenceG[axis]) > thresholdG;
		}
	}
	
	device->motionActive = 0;
	if (!exceeds) device->motionSamples = 0;
	else if (++device->motionSamples >= MAX(1, axesDuration & 0x1FFF))
	{
		device->motionSamples = MAX(1, axesDuration & 0x1FFF);
		if (output >= 1 && output <= 8) device->motionActive = 1U << (output - 1);
		device->registers[FeatureStatusReg] |= device->motionActive;
		/* Functional slope model: reference holds until detection, then follows
		 * detected motion. No Bosch microcode or analog/filter pipeline is run. */
		memcpy(device->motionReferenceG, accelerationG, sizeof(accelerationG));
	}
	
	/* Keep a detected condition until the next feature sample, including when
	 * status is read in non-latched mode. The board can latch both pin edges. */
	timer_mod(device->motionTimer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + motionPeriodNs(device));
	updateIrqs(device);
}

static void configureMotion(Bmi270* device)
{
	timer_del(device->motionTimer);
	device->motionSamples = 0;
	device->motionActive = 0;
	
	const float rangeG = 2U << (device->registers[AccelRangeReg] & 3);
	for (unsigned axis = 0; axis < 3; axis++)
	{
		device->motionReferenceG[axis] = CLAMP(device->accelerationG[axis], -rangeG, rangeG);
	}
	
	const int64_t periodNs = motionPeriodNs(device);
	if (periodNs) timer_mod(device->motionTimer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + periodNs);
	updateIrqs(device);
}

static void resetRegisters(Bmi270* device)
{
	uint8_t* registers = device->registers;
	memset(registers, 0, sizeof(device->registers));
	memset(device->config, 0, sizeof(device->config));
	memset(device->written, 0, sizeof(device->written));
	memset(device->features, 0, sizeof(device->features));
	device->configOffset = device->loadedBytes = 0;
	
	device->nextSampleNs[0] = device->nextSampleNs[1] = 0;
	device->dataReadyUntilNs[0] = device->dataReadyUntilNs[1] = 0;
	timer_del(device->sampleTimer);
	
	registers[0x00] = 0x24; /* BMI270_CHIP_ID */
	registers[StatusReg] = 0x10; /* CMD_RDY; AUX_BUSY is clear. */
	registers[AccelConfReg] = 0xA8;
	registers[AccelRangeReg] = 2;
	registers[GyroConfReg] = 0xA9;
	registers[PowerConfReg] = 3; /* Advanced power save enabled after soft reset. */
	
	device->registerAddress = 0;
	device->registerPhase = false;
	configureMotion(device);
}

static void snapshotTime(Bmi270* device)
{
	uint8_t* registers = device->registers;
	const int64_t nowNs = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
	/* Sensor time ticks at 25.6 kHz; divide first to avoid overflowing ns. */
	const uint32_t ticks = nowNs / 390625 * 10 + nowNs % 390625 * 10 / 390625;
	registers[0x18] = ticks;
	registers[0x19] = ticks >> 8;
	registers[0x1A] = ticks >> 16;
	stw_le_p(registers + 0x22, (registers[PowerControlReg] & 8) ? 1024 : 0x8000); /* 25 C / invalid */
}

static int deviceEvent(I2CSlave* slave, enum i2c_event event)
{
	Bmi270* device = BMI270(slave);
	if (event == I2C_START_RECV) snapshotTime(device);
	if (event == I2C_START_SEND) device->registerPhase = true;
	
	return 0;
}

static uint8_t deviceReceive(I2CSlave* slave)
{
	Bmi270* device = BMI270(slave);
	uint8_t* registers = device->registers;
	const uint8_t address = device->registerAddress++;
	uint8_t value = registers[address];
	
	if (address >= FeatureDataReg && address < AccelConfReg)
	{
		value = device->features[registers[FeaturePageReg] & 7][address - FeatureDataReg];
	}
	else if (address == InitDataReg)
	{
		value = device->config[device->configOffset];
		device->configOffset = (device->configOffset + 1) % sizeof(device->config);
		device->registerAddress = address; /* INIT_DATA is a stream port. */
	}
	else if (address == 0x26)
	{
		value = 0x80; /* Empty FIFO marker; FIFO sampling unsupported. */
		device->registerAddress = address;
	}
	else if (address == FeatureStatusReg || address == DataStatusReg)
	{
		registers[address] = 0; /* Latched interrupt status clears on read. */
		updateIrqs(device);
	}
	else if (address >= AccelDataReg && address <= 0x11)
	{
		registers[StatusReg] &= ~0x80;
	}
	else if (address >= GyroDataReg && address <= 0x17)
	{
		registers[StatusReg] &= ~0x40;
	}
	
	return value;
}

static int deviceSend(I2CSlave* slave, uint8_t value)
{
	Bmi270* device = BMI270(slave);
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
		if (value == 0xB6)
		{
			resetRegisters(device);
		}
		else if (value != 0xB0 && value != 0)
		{
			qemu_log_mask(LOG_UNIMP, "bmi270: command 0x%02x unsupported\n", value);
		}
	}
	else if (address == InitControlReg)
	{
		registers[address] = value & 1;
		if (!(value & 1))
		{
			memset(device->written, 0, sizeof(device->written));
			device->loadedBytes = 0;
			registers[InternalStatusReg] = 0;
		}
		else
		{
			/* Check upload completeness; microcode itself is not executed. */
			registers[InternalStatusReg] = device->loadedBytes == sizeof(device->config) ? 1 : 2;
			if (registers[InternalStatusReg] == 1)
			{
				/* Base-image defaults from FEATURES page 1. Bosch's API preserves
				 * out_conf=7, which routes any-motion to INT_STATUS_0 bit 6. */
				memset(device->features, 0, sizeof(device->features));
				stw_le_p(device->features[1] + 0x0C, 0xE005);
				stw_le_p(device->features[1] + 0x0E, 0x38AA);
			}
		}
		registers[FeatureStatusReg] = registers[DataStatusReg] = 0;
		sampleSensors(device);
		configureMotion(device);
	}
	else if (address == InitDataReg)
	{
		const unsigned offset = device->configOffset;
		const uint8_t mask = 1U << (offset & 7);
		
		if (!(registers[InitControlReg] & 1))
		{
			device->config[offset] = value;
			if (!(device->written[offset / 8] & mask))
			{
				device->written[offset / 8] |= mask;
				device->loadedBytes++;
			}
		}
		device->configOffset = (offset + 1) % sizeof(device->config);
		device->registerAddress = address;
	}
	else if (address >= FeatureDataReg && address < AccelConfReg)
	{
		uint8_t* feature = &device->features[registers[FeaturePageReg]][address - FeatureDataReg];
		if (*feature != value)
		{
			*feature = value;
			if (registers[FeaturePageReg] == 1 && address >= 0x3C) configureMotion(device);
		}
	}
	else if (address == FeaturePageReg || (address >= AccelConfReg && address <= IntDataMapReg) ||
	         address == InitAddressLowReg || address == InitAddressHighReg || (address >= 0x68 && address <= PowerControlReg))
	{
		const uint8_t changed = registers[address] ^ value;
		registers[address] = address == FeaturePageReg ? value & 7 : value;
		if (address == InitAddressLowReg || address == InitAddressHighReg)
		{
			device->configOffset = ((registers[InitAddressLowReg] & 15) | (registers[InitAddressHighReg] << 4)) * 2;
		}
		else if (changed && ((address >= AccelConfReg && address <= GyroRangeReg) || address == PowerControlReg))
		{
			/* Rate changes restart only the affected stream. Range changes take
			 * effect on the next sample without changing either sensor's cadence. */
			if ((address == AccelConfReg || address == GyroConfReg) && (changed & 15))
			{
				device->nextSampleNs[(address - AccelConfReg) / 2] = 0;
			}
			sampleSensors(device);
			if (address == AccelConfReg || address == AccelRangeReg || (address == PowerControlReg && (changed & 4)))
			{
				configureMotion(device);
			}
		}
		else if (address >= Int1ControlReg && address <= IntDataMapReg) updateIrqs(device);
	}
	
	return 0;
}

void bmi270SetMotion(I2CSlave* slave, const float accelerationG[3], const float angularVelocityDps[3])
{
	Bmi270* device = BMI270(slave);
	for (unsigned axis = 0; axis < 3; axis++)
	{
		device->accelerationG[axis] = isfinite(accelerationG[axis]) ? accelerationG[axis] : 0;
		device->angularVelocityDps[axis] = isfinite(angularVelocityDps[axis]) ? angularVelocityDps[axis] : 0;
	}
}

static void instanceInit(Object* obj)
{
	Bmi270* device = BMI270(obj);
	device->accelerationG[2] = 1;
	device->sampleTimer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sampleSensors, device);
	device->motionTimer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sampleMotion, device);
	qdev_init_gpio_out_named(DEVICE(obj), device->irq, "irq", 2);
	resetRegisters(device);
}

static void instanceFinalize(Object* obj)
{
	Bmi270* device = BMI270(obj);
	timer_free(device->sampleTimer);
	timer_free(device->motionTimer);
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
		.name = TYPE_BMI270,
		.parent = TYPE_I2C_SLAVE,
		.instance_size = sizeof(Bmi270),
		.instance_init = instanceInit,
		.instance_finalize = instanceFinalize,
		.class_init = classInit,
	},
};

DEFINE_TYPES(DeviceTypes)
