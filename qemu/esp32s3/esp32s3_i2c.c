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

/* ESP32-S3 FIFO I2C master. Register/command contract: ESP-IDF 5.5.4
 * soc/esp32s3/register/soc/i2c_*.h and hal/esp32s3/include/hal/i2c_ll.h.
 * Transaction-level model; no wire timing, multi-master arbitration,
 * slave mode, non-FIFO execution or external clock stretching. */

#include "qemu/osdep.h"
#include "esp32s3_i2c.h"

#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include "qemu/module.h"

enum
{
	ControlReg = 0x04,
	StatusReg = 0x08,
	TimeoutReg = 0x0C,
	FifoSt = 0x14,
	FifoConf = 0x18,
	DataReg = 0x1C,
	IntRaw = 0x20,
	IntClr = 0x24,
	IntEna = 0x28,
	IntSt = 0x2C,
	Command0Reg = 0x58,
	Date = 0xF8,
	
	CommandCount = 8,
	FifoCapacity = 32,
	
	OpWrite = 1,
	OpStop = 2,
	OpRead = 3,
	OpEnd = 4,
	OpRestart = 6,
	
	CommandAckCheck = 1 << 8,
	CommandAckExpected = 1 << 9,
	CommandAckValue = 1 << 10,
	
	RxWm = 1 << 0,
	TxWm = 1 << 1,
	EndDetect = 1 << 3,
	ByteDone = 1 << 4,
	Complete = 1 << 7,
	Timeout = 1 << 8,
	TransStart = 1 << 9,
	Nack = 1 << 10,
	TxOverflow = 1 << 11,
	RxUnderflow = 1 << 12,
	
	Master = 1 << 4,
	Start = 1 << 5,
	FsmReset = 1 << 10,
	ConfUpdate = 1 << 11,
	
	NonFifo = 1 << 10,
	RxReset = 1 << 12,
	TxReset = 1 << 13,
	FifoProtect = 1 << 14,
};

OBJECT_DECLARE_SIMPLE_TYPE(Esp32s3I2C, ESP32S3_I2C)

struct Esp32s3I2C
{
	SysBusDevice parentObj;
	MemoryRegion mmio;
	qemu_irq irq;
	uint32_t registers[0x100 / 4];
	
	uint8_t tx[FifoCapacity];
	unsigned txHead, txCount;
	
	uint8_t rx[FifoCapacity];
	unsigned rxHead, rxCount;
	
	unsigned commandIndex, byteIndex;
	bool running;
	
	I2CBus* bus;
	uint8_t address;
	bool busy, addressed, addressPhase, read, lastNack;
};

static void updateIrq(Esp32s3I2C* controller)
{
	const uint32_t conf = controller->registers[FifoConf / 4];
	uint32_t* raw = &controller->registers[IntRaw / 4];
	
	*raw &= ~(RxWm | TxWm);
	if (conf & FifoProtect)
	{
		if (controller->rxCount > (conf & 31))
		{
			*raw |= RxWm;
		}
		if (controller->txCount < ((conf >> 5) & 31))
		{
			*raw |= TxWm;
		}
	}
	qemu_set_irq(controller->irq, !!(*raw & controller->registers[IntEna / 4]));
}

static void endBus(Esp32s3I2C* controller)
{
	if (controller->busy) i2c_end_transfer(controller->bus);
	controller->busy = controller->addressed = controller->addressPhase = false;
}

static bool addressDevice(Esp32s3I2C* controller, uint8_t byte)
{
	const uint8_t address = byte >> 1;
	
	/* QEMU keeps the selected slave across repeated START. Reselect only
	 * when the master addresses a different device. */
	if (address != controller->address) i2c_end_transfer(controller->bus);
	controller->address = address;
	controller->read = byte & 1;
	controller->addressPhase = false;
	controller->addressed = i2c_start_transfer(controller->bus, address, controller->read) == 0;
	if (!controller->addressed) i2c_end_transfer(controller->bus);
	
	return controller->addressed;
}

static void runCommands(Esp32s3I2C* controller)
{
	while (controller->running && controller->commandIndex < CommandCount)
	{
		uint32_t* command = &controller->registers[Command0Reg / 4 + controller->commandIndex];
		const unsigned opcode = (*command >> 11) & 7;
		const unsigned length = *command & 255;
		
		switch (opcode)
		{
			case OpRestart: {
				controller->busy = controller->addressPhase = true;
				controller->addressed = false;
				break;
			}
			case OpWrite: {
				while (controller->byteIndex < length)
				{
					if (!controller->txCount)
					{
						goto paused; /* Wait for FIFO refill; do not finish early. */
					}
					
					const uint8_t byte = controller->tx[controller->txHead];
					controller->txHead = (controller->txHead + 1) % FifoCapacity;
					controller->txCount--;
					
					bool ack = false;
					if (controller->addressPhase)
					{
						ack = addressDevice(controller, byte);
					}
					else if (controller->addressed && !controller->read)
					{
						ack = i2c_send(controller->bus, byte) == 0;
					}
					
					controller->lastNack = !ack;
					controller->byteIndex++;
					controller->registers[IntRaw / 4] |= ByteDone;
					
					if ((*command & CommandAckCheck) && controller->lastNack != !!(*command & CommandAckExpected))
					{
						controller->registers[IntRaw / 4] |= Nack;
						controller->running = false;
						endBus(controller);
						goto paused;
					}
				}
				break;
			}
			case OpRead: {
				while (controller->byteIndex < length)
				{
					if (controller->rxCount == FifoCapacity)
					{
						goto paused; /* Resume when software drains the FIFO. */
					}
					
					const uint8_t byte = controller->addressed && controller->read ? i2c_recv(controller->bus) : 0xFF;
					controller->rx[(controller->rxHead + controller->rxCount) % FifoCapacity] = byte;
					controller->rxCount++;
					controller->byteIndex++;
					controller->registers[IntRaw / 4] |= ByteDone;
				}
				
				/* ACK_VAL is the master's final ACK/NACK, not a slave error. */
				if (length && controller->addressed && (*command & CommandAckValue)) i2c_nack(controller->bus);
				break;
			}
			case OpStop: {
				endBus(controller);
				controller->running = false;
				controller->registers[IntRaw / 4] |= Complete;
				break;
			}
			case OpEnd: {
				/* END pauses command execution without STOP; retain bus/pointer. */
				controller->running = false;
				controller->registers[IntRaw / 4] |= EndDetect;
				break;
			}
			default: {
				qemu_log_mask(LOG_UNIMP, "esp32s3-i2c: unsupported S3 command %u\n", opcode);
				controller->registers[IntRaw / 4] |= Timeout;
				controller->running = false;
				endBus(controller);
				goto paused;
			}
		}
		
		*command |= 1U << 31;
		controller->commandIndex++;
		controller->byteIndex = 0;
	}
	
	if (controller->running)
	{
		/* No STOP/END in the command list is not a completed transaction. */
		controller->running = false;
		controller->registers[IntRaw / 4] |= Timeout;
		endBus(controller);
	}

paused:
	updateIrq(controller);
}

static uint64_t i2cRead(void* opaque, hwaddr offset, unsigned size)
{
	Esp32s3I2C* controller = opaque;
	
	switch (offset)
	{
		case StatusReg:
			return controller->lastNack | (controller->busy << 4) | (controller->rxCount << 8) |
			       (3 << 14) | (controller->txCount << 18);
		case FifoSt:
			return controller->rxHead | (((controller->rxHead + controller->rxCount) % FifoCapacity) << 5) |
			       (controller->txHead << 10) | (((controller->txHead + controller->txCount) % FifoCapacity) << 15);
		case DataReg: {
			if (!controller->rxCount)
			{
				controller->registers[IntRaw / 4] |= RxUnderflow;
				updateIrq(controller);
				return 0;
			}
			
			const uint8_t value = controller->rx[controller->rxHead];
			controller->rxHead = (controller->rxHead + 1) % FifoCapacity;
			controller->rxCount--;
			runCommands(controller);
			return value;
		}
		case IntClr:
			return 0;
		case IntRaw:
		case IntSt: {
			updateIrq(controller);
			return controller->registers[IntRaw / 4] & (offset == IntSt ? controller->registers[IntEna / 4] : UINT32_MAX);
		}
		default: {
			if (offset < sizeof(controller->registers))
			{
				return controller->registers[offset / 4];
			}
			/* Direct RAM inspection only; non-FIFO execution is unsupported. */
			if (offset >= 0x100 && offset < 0x180)
			{
				return controller->tx[(offset - 0x100) / 4];
			}
			return controller->rx[(offset - 0x180) / 4];
		}
	}
}

static void i2cWrite(void* opaque, hwaddr offset, uint64_t value, unsigned size)
{
	Esp32s3I2C* controller = opaque;
	
	switch (offset)
	{
		case ControlReg: {
			controller->registers[ControlReg / 4] = value & 0x7FFF & ~(Start | FsmReset | ConfUpdate);
			if (value & FsmReset)
			{
				controller->running = false;
				controller->commandIndex = controller->byteIndex = 0;
				controller->lastNack = false;
				endBus(controller);
			}
			
			if (value & Start)
			{
				controller->commandIndex = controller->byteIndex = 0;
				for (unsigned i = 0; i < CommandCount; i++)
				{
					controller->registers[Command0Reg / 4 + i] &= ~(1U << 31);
				}
				if (!(value & Master) || (controller->registers[FifoConf / 4] & NonFifo))
				{
					qemu_log_mask(LOG_UNIMP, "esp32s3-i2c: only FIFO master mode supported\n");
					controller->registers[IntRaw / 4] |= Timeout;
					controller->running = false;
					endBus(controller);
				}
				else
				{
					controller->running = true;
					controller->registers[IntRaw / 4] |= TransStart;
					runCommands(controller);
				}
			}
			break;
		}
		case FifoConf: {
			controller->registers[offset / 4] = value & 0x7FFF;
			if (value & RxReset)
			{
				controller->rxHead = controller->rxCount = 0;
			}
			if (value & TxReset)
			{
				controller->txHead = controller->txCount = 0;
			}
			break;
		}
		case DataReg: {
			if (controller->txCount == FifoCapacity)
			{
				controller->registers[IntRaw / 4] |= TxOverflow;
			}
			else
			{
				controller->tx[(controller->txHead + controller->txCount) % FifoCapacity] = value;
				controller->txCount++;
				runCommands(controller);
			}
			break;
		}
		case IntClr: {
			controller->registers[IntRaw / 4] &= ~value;
			break;
		}
		case IntEna: {
			controller->registers[offset / 4] = value & 0x3FFFF;
			break;
		}
		case StatusReg:
		case FifoSt:
		case IntRaw:
		case IntSt:
			break;
		case Command0Reg ... Command0Reg + (CommandCount - 1) * 4: {
			controller->registers[offset / 4] = value & 0x3FFF;
			break;
		}
		default: {
			if (offset <= 0x84 || offset == Date)
			{
				controller->registers[offset / 4] = value;
				if (offset == 0x80)
				{
					controller->registers[offset / 4] &= ~1U; /* SCL bus-clear request self-clears. */
				}
			}
			else if (offset >= 0x100 && offset < 0x180)
			{
				controller->tx[(offset - 0x100) / 4] = value;
			}
			break;
		}
	}
	
	updateIrq(controller);
}

static void i2cReset(DeviceState* dev)
{
	Esp32s3I2C* controller = ESP32S3_I2C(dev);
	
	/* An ESP reset does not power-cycle the external devices or battery RTC. */
	memset(controller->registers, 0, sizeof(controller->registers));
	memset(controller->tx, 0, sizeof(controller->tx));
	memset(controller->rx, 0, sizeof(controller->rx));
	controller->txHead = controller->txCount = controller->rxHead = controller->rxCount = 0;
	controller->commandIndex = controller->byteIndex = 0;
	controller->running = controller->lastNack = false;
	endBus(controller);
	
	controller->registers[ControlReg / 4] = 0x20B;
	controller->registers[TimeoutReg / 4] = 16;
	controller->registers[FifoConf / 4] = FifoProtect | (4 << 5) | 11;
	controller->registers[0x40 / 4] = controller->registers[0x44 / 4] = 8;
	controller->registers[0x48 / 4] = controller->registers[0x4C / 4] = 8;
	controller->registers[0x50 / 4] = 0x300;
	controller->registers[0x54 / 4] = 1U << 21;
	controller->registers[0x78 / 4] = controller->registers[0x7C / 4] = 16;
	controller->registers[Date / 4] = 537330177;
	
	updateIrq(controller);
}

static const MemoryRegionOps I2cOps = {
	.read = i2cRead,
	.write = i2cWrite,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = { .min_access_size = 4, .max_access_size = 4 },
	.impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void instanceInit(Object* obj)
{
	Esp32s3I2C* controller = ESP32S3_I2C(obj);
	memory_region_init_io(&controller->mmio, obj, &I2cOps, controller, TYPE_ESP32S3_I2C, 0x200);
	sysbus_init_mmio(SYS_BUS_DEVICE(obj), &controller->mmio);
	sysbus_init_irq(SYS_BUS_DEVICE(obj), &controller->irq);
}

static void realize(DeviceState* dev, Error** errp)
{
	Esp32s3I2C* controller = ESP32S3_I2C(dev);
	if (!controller->bus) controller->bus = i2c_init_bus(dev, "i2c");
	i2cReset(dev);
}

static Property Properties[] = {
	DEFINE_PROP_LINK("bus", Esp32s3I2C, bus, TYPE_I2C_BUS, I2CBus*),
	DEFINE_PROP_END_OF_LIST(),
};

static void classInit(ObjectClass* klass, void* data)
{
	DeviceClass* device = DEVICE_CLASS(klass);
	device->realize = realize;
	device_class_set_props(device, Properties);
	device_class_set_legacy_reset(device, i2cReset);
}

static const TypeInfo DeviceTypes[] = {
	{
		.name = TYPE_ESP32S3_I2C,
		.parent = TYPE_SYS_BUS_DEVICE,
		.instance_size = sizeof(Esp32s3I2C),
		.instance_init = instanceInit,
		.class_init = classInit,
	},
};

DEFINE_TYPES(DeviceTypes)
