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

#pragma once

#include "audio/audio.h"
#include "hw/i2c/i2c.h"
#include "qemu/fifo8.h"

#define TYPE_ES8311 "es8311"
OBJECT_DECLARE_SIMPLE_TYPE(Es8311, ES8311)

enum
{
	Es8311ResetReg = 0x00,
	Es8311ClockReg = 0x01,
	Es8311AdcClockReg = 0x07,
	Es8311DacFormatReg = 0x09,
	Es8311AdcFormatReg = 0x0A,
	Es8311SystemPowerReg = 0x0D,
	Es8311AnalogPowerReg = 0x0E,
	Es8311DacPowerReg = 0x12,
	Es8311AnalogInputReg = 0x14,
	Es8311AdcScaleReg = 0x16,
	Es8311AdcVolumeReg = 0x17,
	Es8311DacMuteReg = 0x31,
	Es8311DacVolumeReg = 0x32,
	Es8311RoutingReg = 0x44,
};

struct Es8311
{
	I2CSlave parentObj;
	uint8_t registers[256], registerAddress;
	bool registerPhase, powered, amplifier;
	
	QEMUSoundCard card;
	
	SWVoiceOut* output;
	Fifo8 playback;
	unsigned outputRate;
	double outputGain;
	bool outputActive;
	
	SWVoiceIn* input;
	Fifo8 capture;
	unsigned inputRate;
	double inputGain;
	bool inputActive, capturePrimed, microphone;
};

void es8311UpdateAudio(Es8311* device);

void es8311SetRate(Object* codec, bool input, unsigned rate);
void es8311Transfer(Object* codec, bool input, int32_t frames[][2], unsigned count);
