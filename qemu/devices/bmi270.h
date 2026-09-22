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

#include "hw/i2c/i2c.h"

#define TYPE_BMI270 "bmi270"

/* Named GPIO outputs "irq"[0/1] are INT1/INT2 electrical levels, not assertion
 * flags. Disabled outputs resolve low; open-drain highs assume a pull-up.
 * Initialize the board's cached INT levels to zero when connecting these pins.
 * MCU reset preserves BMI state; only instance initialization or CMD=0xB6 resets it.
 * Motion inputs and both interrupt timers use the virtual clock under the BQL;
 * enabled sensors/features run without I2C reads, including while the CPU idles. */
void bmi270SetMotion(I2CSlave* slave, const float accelerationG[3], const float angularVelocityDps[3]);
