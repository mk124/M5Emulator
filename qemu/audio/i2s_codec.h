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

#include "qom/object.h"

#define TYPE_I2S_CODEC "i2s-codec"
typedef struct I2sCodecClass I2sCodecClass;
DECLARE_CLASS_CHECKERS(I2sCodecClass, I2S_CODEC, TYPE_I2S_CODEC)

enum
{
	I2sBlockFrames = 256
};

struct I2sCodecClass
{
	InterfaceClass parentClass;
	
	/* PCM at frame boundaries, signed full-scale 32-bit left/right samples.
	 * input means codec -> controller; rate zero stops that direction.
	 * Transfers contain at most I2sBlockFrames and never block guest DMA. */
	void (*setRate)(Object* codec, bool input, unsigned rate);
	void (*transfer)(Object* codec, bool input, int32_t frames[][2], unsigned count);
};
