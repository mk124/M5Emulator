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

/* ES8311 control port, supply and host audio device lifecycle. */

#include "qemu/osdep.h"
#include "es8311.h"

#include "hw/qdev-properties.h"
#include "qemu/module.h"

#include "../audio/i2s_codec.h"

static void resetRegisters(Es8311* device)
{
	static const uint8_t Defaults[256] = {
		[Es8311ResetReg] = 0x1F,
		[Es8311ClockReg] = 0x30, [0x03] = 0x10, [0x04] = 0x10, [0x06] = 3, [0x08] = 0xFF,
		[Es8311SystemPowerReg] = 0xFC, [Es8311AnalogPowerReg] = 0x6A, [Es8311DacPowerReg] = 2, [Es8311AnalogInputReg] = 0x10,
		[Es8311AdcScaleReg] = 4, [0x1B] = 0x0C, [0x1C] = 0x4C, [0x37] = 8,
		[0xFD] = 0x83, [0xFE] = 0x11, [0xFF] = 1,
	};
	
	memcpy(device->registers, Defaults, sizeof(Defaults));
	device->registerAddress = 0;
	device->registerPhase = true;
}

static void powerInput(void* opaque, int line, int level)
{
	Es8311* device = opaque;
	if (device->powered == !!level) return;
	
	device->powered = !!level;
	if (device->powered) resetRegisters(device);
	es8311UpdateAudio(device);
}

static void amplifierInput(void* opaque, int line, int level)
{
	Es8311* device = opaque;
	if (device->amplifier == !!level) return;
	
	device->amplifier = !!level;
	es8311UpdateAudio(device);
}

static int deviceEvent(I2CSlave* slave, enum i2c_event event)
{
	Es8311* device = ES8311(slave);
	if (!device->powered && (event == I2C_START_SEND || event == I2C_START_RECV)) return -1;
	if (event == I2C_START_SEND) device->registerPhase = true;
	
	return 0;
}

static uint8_t deviceReceive(I2CSlave* slave)
{
	Es8311* device = ES8311(slave);
	return device->registers[device->registerAddress++];
}

static int deviceSend(I2CSlave* slave, uint8_t value)
{
	Es8311* device = ES8311(slave);
	if (device->registerPhase)
	{
		device->registerAddress = value;
		device->registerPhase = false;
		return 0;
	}
	
	uint8_t* registers = device->registers;
	const uint8_t address = device->registerAddress++;
	/* Digital reset excludes the I2C register bank. */
	if (address <= 0x1C || (address >= 0x31 && address <= 0x37) || address == 0x44 || address == 0x45)
	{
		registers[address] = value;
		es8311UpdateAudio(device);
	}
	
	return 0;
}

static void instanceInit(Object* obj)
{
	Es8311* device = ES8311(obj);
	fifo8_create(&device->playback, 4096 * sizeof(int32_t));
	fifo8_create(&device->capture, 4096 * sizeof(int32_t));
	
	qdev_init_gpio_in_named(DEVICE(obj), powerInput, "power", 1);
	qdev_init_gpio_in_named(DEVICE(obj), amplifierInput, "amplifier", 1);
	resetRegisters(device);
}

static void instanceFinalize(Object* obj)
{
	Es8311* device = ES8311(obj);
	fifo8_destroy(&device->playback);
	fifo8_destroy(&device->capture);
}

static void deviceRealize(DeviceState* dev, Error** errp)
{
	AUD_register_card("ES8311", &ES8311(dev)->card, errp);
}

static void deviceUnrealize(DeviceState* dev)
{
	Es8311* device = ES8311(dev);
	AUD_close_out(&device->card, device->output);
	AUD_close_in(&device->card, device->input);
	AUD_remove_card(&device->card);
}

static Property Properties[] = {
	DEFINE_AUDIO_PROPERTIES(Es8311, card),
	DEFINE_PROP_BOOL("microphone", Es8311, microphone, false),
	DEFINE_PROP_END_OF_LIST(),
};

static void classInit(ObjectClass* klass, void* data)
{
	DeviceClass* device = DEVICE_CLASS(klass);
	device->realize = deviceRealize;
	device->unrealize = deviceUnrealize;
	device_class_set_props(device, Properties);
	
	I2CSlaveClass* slave = I2C_SLAVE_CLASS(klass);
	slave->event = deviceEvent;
	slave->recv = deviceReceive;
	slave->send = deviceSend;
	
	I2sCodecClass* codec = I2S_CODEC_CLASS(klass);
	codec->setRate = es8311SetRate;
	codec->transfer = es8311Transfer;
}

static const TypeInfo DeviceTypes[] = {
	{
		.name = TYPE_ES8311,
		.parent = TYPE_I2C_SLAVE,
		.instance_size = sizeof(Es8311),
		.instance_init = instanceInit,
		.instance_finalize = instanceFinalize,
		.class_init = classInit,
		.interfaces = (InterfaceInfo[]) { { TYPE_I2S_CODEC }, {} },
	},
};

DEFINE_TYPES(DeviceTypes)
