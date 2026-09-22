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

#include "qemu/osdep.h"

#include "BleHle.h"

#include "cpu.h"
#include "exec/cpu-common.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "exec/tb-flush.h"
#include "qapi/error.h"
#include "qemu/timer.h"

void bleHleInitMemory(void* owner, void* sysmem)
{
	if (!bleHleEnabled()) return;
	
	static MemoryRegion code;
	memory_region_init_ram(&code, owner, "esp32s3.ble-hle", 4096, &error_fatal);
	uint8_t* bytes = memory_region_get_ram_ptr(&code);
	
	/* A new emulator-owned task entry, separate from firmware and chip ROM. */
	const uint8_t entry[] = { 0x36, 0x81, 0x00 }; /* ENTRY a1, 64 */
	memcpy(bytes + HleTaskEntry - HleReturn, entry, sizeof(entry));
	
	memory_region_set_readonly(&code, true);
	memory_region_add_subregion(sysmem, HleReturn, &code);
}

bool bleHleTryRead(void* guest, uint32_t address, void* bytes, size_t length)
{
	CPUXtensaState* env = guest;
	return cpu_memory_rw_debug(env_cpu(env), address, bytes, length, false) == 0;
}

void bleHleRead(void* guest, uint32_t address, void* bytes, size_t length)
{
	CPUXtensaState* env = guest;
	if (cpu_memory_rw_debug(env_cpu(env), address, bytes, length, false))
	{
		fprintf(stderr, "BLE HLE: invalid guest read %08X/%zu\n", address, length);
		abort();
	}
}

void bleHleWrite(void* guest, uint32_t address, const void* bytes, size_t length)
{
	CPUXtensaState* env = guest;
	
	/* The HLE bridge only writes data, including legitimate task stack arguments. */
	if (address < 0x3FC80000 || (uint64_t) address + length > 0x3FD00000)
	{
		fprintf(stderr, "BLE HLE: write outside internal data RAM %08X/%zu\n", address, length);
		abort();
	}
	if (cpu_memory_rw_debug(env_cpu(env), address, (void*) bytes, length, true))
	{
		abort();
	}
}

uint64_t bleHleTimeNs(void)
{
	return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

void HELPER(ble_hle)(CPUXtensaState* env, uint32_t pc)
{
	if (pc == HleRomDataInit)
	{
		if (qemu_tcg_mttcg_enabled())
		{
			fprintf(stderr, "BLE HLE: use -accel tcg,thread=single\n");
			abort();
		}
		
		bleHleDiscover(env);
		uint8_t code[6];
		bleHleRead(env, pc, code, sizeof(code));
		const uint8_t trampoline[] = { 0x91, 0xFF, 0xFF, 0xA0, 0x09, 0x00 };
		if (memcmp(code, trampoline, sizeof(code)))
		{
			fprintf(stderr, "BLE HLE: unsupported ESP32-S3 ROM trampoline\n");
			exit(1);
		}
		
		bleHleRead(env, pc - 4, &env->regs[9], sizeof(uint32_t));
		env->pc = pc + 3;
		tb_flush(env_cpu(env));
		return;
	}
	
	if (pc == HleReturn)
	{
		/* Keep RETW restartable if a register-window underflow occurs. */
		HELPER(test_ill_retw)(env, pc);
		HELPER(test_underflow_retw)(env, pc);
		const uint32_t returnAddress = env->regs[0];
		env->sregs[WINDOW_START] &= ~(1u << env->sregs[WINDOW_BASE]);
		HELPER(retw)(env, returnAddress);
		env->pc = (pc & 0xC0000000) | (returnAddress & 0x3FFFFFFF);
		return;
	}
	
	const BleHleStep step = bleHleStep(env, bleHleOperation(pc), env->regs);
	if (step.continuation)
	{
		for (unsigned i = 0; i < 6; ++i) env->regs[10 + i] = step.args[i];
		const uint32_t stackArgument = cpu_to_le32(step.args[6]);
		bleHleWrite(env, env->regs[1], &stackArgument, sizeof(stackArgument));
		env->sregs[PS] = (env->sregs[PS] & ~PS_CALLINC) | (2 << PS_CALLINC_SHIFT);
		env->regs[8] = 0x80000000 | (step.continuation & 0x3FFFFFFF);
	}
	else
	{
		env->regs[2] = step.result;
	}
	env->pc = step.target;
}
