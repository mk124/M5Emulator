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

#include "hw/qdev-core.h"

#define TYPE_CO5300 "co5300"

uint64_t co5300Updates(DeviceState* dev);
uint32_t co5300CompletedWindows(DeviceState* dev);
uint64_t co5300ActiveRefreshes(DeviceState* dev);

void co5300SetBrightnessSimulation(DeviceState* dev, bool enabled);

// The destination holds width * height RGB565 pixels, using the configured viewport.
void co5300ReadPixels(DeviceState* dev, uint16_t* destination);
