/*
  SPDX-License-Identifier: GPL-2.0-or-later
  ONE NES emulator
  Copyright (C) 1999-2026 Chris Teague

  ONE is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  ONE is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with ONE; if not, see <https://www.gnu.org/licenses/>.
*/
#ifndef cpu_h_defined
#define cpu_h_defined

#include <stdint.h>
#include <stdbool.h>
#include "libco/libco.h"

#define CPU_ONE_CYCLE (12) /* one CPU cycle is 12 cycles of the reference clock */

void cpu_init(void);
void cpu_power_on(void);
void cpu_reset();
// Override the CPU state right after the reset sequence (e.g. nestest automation mode at $C000).
// Pass -1 for any value to leave it as reset produced it.
void cpu_set_start_state(int pc, int a, int x, int y, int s, int p);
// Press RESET at the first instruction boundary at or after this CPU cycle (0 = never).
void cpu_set_soft_reset_cycle(uint64_t cycle);
void cpu_request_soft_reset(void); // press RESET at the next instruction boundary (debugger)
uint8_t cpu_read(uint16_t address);
// Last value seen on the CPU data bus (for unmapped reads in mappers).
uint8_t cpu_get_open_bus(void);
void cpu_write(uint16_t address, uint8_t value);
void cpu_nmi_request(bool request);
void cpu_irq_request(bool request, uint8_t mask);
bool cpu_irq_requested(uint8_t mask); // is one of the requests in MASK asserted
cothread_t cpu_get_thread();
void cpu_cleanup();
void cpu_step();
bool cpu_clock_is_odd_cycle(void);
uint64_t cpu_get_cycle(); // number of cycles since reset
bool cpu_ppu_turn(void);  // the PPU is about to hand the CPU its turn: true if it is to keep running (cpu.c)
uint8_t cpu_peek(uint16_t address);
uint8_t *cpu_internal_ram(void); // the 2 KB of CPU RAM at $0000 (libretro: RetroAchievements)
#define CPU_INTERNAL_RAM_SIZE 2048
void cpu_set_trace_sample(uint32_t every,
                          uint32_t seed); // (--trace-sample) with PPU catch-up: a line at about every N-th instruction
void cpu_set_trace(bool enable);          // per-instruction Mesen-format trace on stderr (-t); off by default
bool cpu_trace_active(void);              // is -t on (the PPU then keeps the hardware's sprite limit)
void cpu_request_stop(
    void); // stop emulation at the next instruction boundary (window closed, ...)  // side-effect-free read (used by -D); CPU RAM only without DEBUGGING_ENABLED

#ifdef DEBUGGING_ENABLED
uint8_t cpu_debug_read(uint16_t address);
uint16_t cpu_debug_get_pc();
uint8_t cpu_debug_get_reg_a();
uint8_t cpu_debug_get_reg_x();
uint8_t cpu_debug_get_reg_y();
uint8_t cpu_debug_get_reg_p();
uint8_t cpu_debug_get_sp();
uint64_t cpu_debug_get_cycle();
uint8_t cpu_debug_read_byte(uint16_t address);
uint16_t cpu_debug_read_word(uint16_t address);
uint16_t cpu_debug_read_word_zp(uint8_t address);
bool cpu_debug_write(uint16_t address, uint8_t value); // internal RAM and PRG-RAM ($6000-$7FFF) only
void cpu_debug_set_pc(uint32_t new_pc);
void cpu_debug_set_reg_a(uint8_t new_a);
void cpu_debug_set_reg_x(uint8_t new_x);
void cpu_debug_set_reg_y(uint8_t new_y);
void cpu_debug_set_reg_p(uint8_t new_p);
void cpu_debug_set_reg_sp(uint8_t new_sp);
#endif // DEBUGGING_ENABLED
#endif
