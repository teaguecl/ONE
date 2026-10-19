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
#ifndef rp2a03_h_defined
#define rp2a03_h_defined

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

void rp2a03_init();
void rp2a03_cleanup();
void rp2a03_power_on();
void rp2a03_reset();
void rp2a03_write(uint16_t address, uint8_t value);
uint8_t rp2a03_read(uint16_t address);
bool rp2a03_dmc_dma_pending(void);
bool rp2a03_oam_dma_pending(void);
void rp2a03_oam_dma(uint16_t halt_address);
uint16_t rp2a03_dmc_dma_address(void);
void rp2a03_dmc_dma_complete(uint8_t value);
uint8_t rp2a03_debug_read(uint16_t address);

// The sound channels, for scripts (FCEUX's sound.get())
typedef struct {
    uint16_t period; // the timer period from the registers: 11 bits (pulse, triangle), 0-15 (noise: $400E)
    uint8_t volume;  // 0-15: the constant volume or the envelope's (pulse, noise)
    uint8_t duty;    // pulse: 0-3
    uint8_t length;  // length counter
    uint8_t linear;  // triangle: linear counter
    bool muted;      // pulse: period < 8 or sweep target > $7FF
    bool short_mode; // noise: mode flag
} rp2a03_channel_state;
typedef struct {
    rp2a03_channel_state pulse[2], triangle, noise;
    uint8_t dmc_rate;       // $4010 bits 0-3
    bool dmc_loop;          // $4010 bit 6
    uint16_t dmc_address;   // sample start ($4012)
    uint16_t dmc_length;    // sample length in bytes ($4013)
    uint16_t dmc_remaining; // bytes left to fetch
    uint8_t dmc_output;     // 7-bit output level
} rp2a03_debug_state;
void rp2a03_debug_state_get(rp2a03_debug_state *out);
// The last OAM DMA ($4014): the page it copied (page $02 = $0200-$02FF) and the frame (ppu_debug_get_frame_num()).
// false: none since power-on.
bool rp2a03_debug_oam_dma(uint8_t *page, uint32_t *frame);

// APU state log (--apu-log FILE): one line per frame, in the format of tools/mesen/apu_log.lua, so the two can be
// compared field by field (tools/apu_compare.py). rp2a03_frame_end() is called by the PPU once per frame.
void rp2a03_set_log(FILE *out);
void rp2a03_frame_end(void);
// Expansion audio (a mapper's channels, mapper_info_t::cpu_tick): its output in mixer units (like the APU's own, 0.0 .. 1.0),
// added to the 2A03's. Call when it changes; it stays until the next call.
void rp2a03_set_expansion(double level);

#endif
