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

#include "hw/dma/esp_gdma.h"
#include "hw/sysbus.h"

#include "esp32s3_i2s_stream.h"

#define TYPE_ESP32S3_I2S "esp32s3-i2s"

enum
{
	IntRaw = 0x0C,
	IntSt = 0x10,
	IntEna = 0x14,
	IntClr = 0x18,
	RxConf = 0x20,
	TxConf = 0x24,
	RxConf1 = 0x28,
	RxClk = 0x30,
	TxClk = 0x34,
	RxDiv = 0x38,
	RxTdm = 0x50,
	RxEof = 0x64,
	State = 0x6C,
	Date = 0x80,
	
	Reset = 1 << 0,
	FifoReset = 1 << 1,
	Start = 1 << 2,
	Slave = 1 << 3,
	Mono = 1 << 5,
	TxRepeatMono = 1 << 6,
	BigEndian = 1 << 7,
	Update = 1 << 8,
	RxMonoFirst = 1 << 9,
	PcmBypass = 1 << 12,
	Fill24 = 1 << 16,
	LsbFirst = 1 << 18,
	Tdm = 1 << 19,
	Pdm = 1 << 20,
	ShareClock = 1 << 27,
	
	ClockActive = 1 << 26,
	ClockEnable = 1 << 29,
};

OBJECT_DECLARE_SIMPLE_TYPE(Esp32s3I2S, ESP32S3_I2S)

struct Esp32s3I2S
{
	SysBusDevice parentObj;
	MemoryRegion mmio;
	uint32_t registers[0x84 / 4];
	
	ESPGdmaState* gdma;
	Object* codec;
	Esp32s3I2SStream stream[ESP_GDMA_CONF_COUNT];
};

double esp32s3I2sBytePeriodNs(const Esp32s3I2S* i2s, unsigned direction);
void esp32s3I2sUpdateAudio(Esp32s3I2S* i2s, unsigned direction);
void esp32s3I2sTransferAudio(Esp32s3I2S* i2s, unsigned direction, uint8_t* samples, unsigned bytes);
