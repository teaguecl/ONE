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
#ifndef ppu_h_defined
#define ppu_h_defined

#include <stdint.h>
#include <stdbool.h>

void ppu_init(void);
void ppu_power_on();
void ppu_reset();
void ppu_soft_reset(void);
void ppu_cleanup();
uint8_t ppu_read(uint16_t address);
void ppu_write(uint16_t address, uint8_t value);
void ppu_set_read_addr(uint16_t address);
bool ppu_add_cycles(uint32_t cycles);
// The console's 2KB of name table RAM (CIRAM), for a mapper that uses it as pattern data too (Namco 163); OFFSET is taken mod 2KB
uint8_t ppu_ciram_read(unsigned offset);
void ppu_ciram_write(unsigned offset, uint8_t value);
uint64_t ppu_dot_now(void);      // the PPU's time in dots (mapper hooks: ppu_fetch, read_exp)
bool ppu_a12_from_sprite(void);  // the A12 change the mapper hook is called for comes from a sprite pattern fetch
uint32_t ppu_dots_skipped(void); // odd-frame dots skipped so far (ppu_dot_now() counts them as time)
#ifdef FEATURE_PPU_CATCHUP
int32_t ppu_catchup_limit(void); // PPU clocks the CPU may run ahead (cpu.c)
int32_t ppu_clock_debt(void);    // PPU clocks the CPU has run ahead
#endif
void ppu_set_apu_log(bool active); // call rp2a03_frame_end() once per frame (the --apu-log point)

// debug
uint8_t ppu_debug_read(uint16_t address);
uint8_t ppu_debug_read_regs(uint16_t address);
void ppu_debug_get_trace_position(int *trace_scanline, int *trace_dot);
void ppu_set_open_bus(uint8_t value);
// "Disable Sprite Limit": draw every sprite of a line, not just 8 (not with -t, nor with ONE_HARDWARE_SPRITES set)
void ppu_set_sprite_limit(bool disable);
bool ppu_sprite_limit_forced(void); // ONE_HARDWARE_SPRITES is set: the setting is ignored (the tests set it)
uint8_t ppu_debug_read_oam(uint8_t address);
uint8_t ppu_get_ppustatus();
uint8_t ppu_get_ppuctrl();
uint8_t ppu_get_ppumask();
uint16_t ppu_get_vram_addr();
uint32_t ppu_debug_get_clk();
uint32_t ppu_debug_get_scanline();
uint32_t ppu_debug_get_scanline_clock();
uint8_t ppu_get_ppusar();
uint8_t ppu_get_ppusdr();
uint32_t ppu_debug_get_frame_num();
uint16_t ppu_debug_get_v(void); // the VRAM address ("loopy v")
uint16_t ppu_debug_get_t(void);
uint8_t ppu_debug_get_fine_x(void);
bool ppu_debug_get_w(void);
uint8_t ppu_debug_read_palette(uint8_t index);
void ppu_debug_write_oam(uint8_t address, uint8_t value);
uint8_t ppu_debug_read_oam2(uint8_t address); // secondary OAM
// The last sprite 0 hit: the picture line and pixel where it happened, and the frame (ppu_debug_get_frame_num()).
// false: none since power-on.
bool ppu_debug_sprite0_hit(int *line, int *x, uint32_t *frame);
void ppu_debug_write_oam2(uint8_t address, uint8_t value);
void ppu_debug_write_palette(uint8_t index, uint8_t value);
// The scroll each picture line (0-239) started with, for one-debugger's background viewer: loopy v and fine x as the
// line's first tiles were fetched (dot 321 of the line before it, or of the pre-render line for line 0), PPUMASK and
// PPUCTRL then, and where the background's pattern bytes came from (games switch CHR banks mid-frame). Lines the PPU has not reached yet this frame still hold the previous frame's. Only the debugger
// build of the core (ONE_DEBUGGER) records them; elsewhere they stay zero.
typedef struct {
    uint16_t v;
    uint8_t fine_x;
    uint8_t mask;
    uint8_t ctrl;
    int32_t
        bg_chr[4]; // the background pattern table's 1KB pages (PPUCTRL bit 4): CHR offsets, mapper_debug_chr_offset()
} ppu_line_scroll;
const ppu_line_scroll *ppu_debug_line_scroll(void); // 240 entries
#endif
