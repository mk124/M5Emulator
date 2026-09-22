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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Only ROM exports and emulator-owned continuations have fixed addresses. */
enum
{
	HleReturn = 0x40800000 + 0,
	HleAllocated = 0x40800000 + 4,
	HleSemaphoreCreated = 0x40800000 + 8,
	HleTaskCreated = 0x40800000 + 12,
	HleTaskCreateFailed = 0x40800000 + 16,
	HleTaskDeleted = 0x40800000 + 20,
	HleSemaphoreDeleted = 0x40800000 + 24,
	HleFreed = 0x40800000 + 28,
	HleReceiveReturned = 0x40800000 + 32,
	HleReadyReturned = 0x40800000 + 36,
	HleDelayReturned = 0x40800000 + 40,
	
	HleTaskEntry = 0x40800080,
	HleTaskPoll = 0x40800083,
	HleRomDataInit = 0x40002AA8,
	
	HleInit = 256,
	HleDeinit = 257,
	HleEnable = 258,
	HleDisable = 259,
	HleMode = 260,
	HleVersion = 261,
	HleAvailable = 262,
	HleSend = 263,
	HleRegister = 264,
	HleSleepEnable = 265,
	HleSleepMode = 266,
	HleWakeup = 267,
	HleWakeupRequesting = 268,
	HlePowerSet = 269,
	HlePowerGet = 270,
	HlePowerActive = 271,
	HleOffloadRegister = 272,
	HleOffloadDeregister = 273,
	HlePllTrack = 274,
	HleAdvFlowControl = 275,
	HleClearLegacyAdv = 276,
	HleDuplicateExceptions = 277,
	HleChannelSelection = 278,
	HlePhyInit = 279,
	HlePhyWakeup = 280,
	HlePhyClose = 281,
	HlePhyTemperatureOff = 282,
	HlePhyVersion = 283,
	HlePhyCloseImpl = 284,
	HlePhyWakeupImpl = 285,
	HlePhyTrack = 286,
};

typedef struct BleHleStep
{
	uint32_t target, continuation;
	uint32_t args[7];
	uint32_t result;
} BleHleStep;

#ifdef __cplusplus
extern "C" {
#endif

bool bleHleEnabled(void);
void bleHleReset(void);
BleHleStep bleHleStep(void* guest, uint32_t operation, const uint32_t* registers);

void bleHleDiscover(void* guest);
void bleHleResetHooks(void);
uint32_t bleHleGattList(void* guest);
uint32_t bleHleOperation(uint32_t pc);
bool bleHleHandles(uint32_t pc);

void bleHleInitMemory(void* owner, void* sysmem);
bool bleHleTryRead(void* guest, uint32_t address, void* bytes, size_t length);
void bleHleRead(void* guest, uint32_t address, void* bytes, size_t length);
void bleHleWrite(void* guest, uint32_t address, const void* bytes, size_t length);
uint64_t bleHleTimeNs(void);

#ifdef __cplusplus
}
#endif
