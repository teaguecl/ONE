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

/* The hardware of Konami's VRC2 and VRC4 (iNES mappers 21, 22, 23 and 25): 8KB PRG banks, eight 1KB CHR banks, 8KB of work RAM at
 * $6000, mirroring, and (VRC4) a CPU-cycle / scanline IRQ. The boards differ only in which CPU address lines select a register
 * within a group of four ($x000-$x003) and in the VRC2/VRC4 features; a mapper (mapper_vrc4ac.c ...) names its lines in a vrc24_board_t.
 * https://www.nesdev.org/wiki/VRC2_and_VRC4 */
#ifndef mapper_vrc24_h_defined
#define mapper_vrc24_h_defined

#include <stdbool.h>
#include <stdint.h>
#include "ines.h"
#include "mapper.h"

typedef struct {
    const char *name;
    uint16_t
        reg0_lines; // CPU address lines (a mask) that ORed make register-select bit 0: the 4 registers are $x000 + (bit1 << 1 | bit0)
    uint16_t reg1_lines; // ... and bit 1
    bool vrc4;      // IRQ, PRG swap mode, 2-bit mirroring ($9000); else a VRC2: one mirroring bit, none of the others
    bool chr_shift; // VRC2a: the CHR bank number is shifted right by one
} vrc24_board_t;

// BOARD describes the ROM's wiring (the NES 2.0 submapper picks it in the mapper's own file); sav_data restores battery RAM
bool mapper_vrc24_init(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data,
                       const vrc24_board_t *board);
unsigned vrc24_submapper(const ines_header *h); // NES 2.0 submapper, 0 for iNES 1.0 (and for a file ONE cannot decode)

#endif // mapper_vrc24_h_defined
