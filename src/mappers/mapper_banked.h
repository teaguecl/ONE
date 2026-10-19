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

/* The hardware of the simple register boards with 8KB PRG-ROM banks and 1KB CHR-ROM banks (Taito TC0190, Konami VRC1,
 * Irem G-101): four 8KB PRG slots at $8000-$FFFF, eight 1KB CHR slots, and mirroring, all set by CPU writes to
 * $8000-$FFFF. No PRG-RAM, no IRQ. A mapper (mapper_taito_tc0190.c ...) decodes the writes in its write_reg and calls the
 * banked_select_* functions; the board wraps bank numbers around the ROM's size. */
#ifndef mapper_banked_h_defined
#define mapper_banked_h_defined

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ines.h"
#include "mapper.h"

struct state_io;

typedef struct {
    const char *name;
    bool
        mesen_work_ram; // MESEN_COMPATIBILITY: 8KB of work RAM at $6000-$7FFF, which Mesen gives this board (the real one has none)
    void (*write_reg)(uint16_t address, uint8_t value); // a CPU write to $8000-$FFFF
    void (*power_on)(void);             // the registers at power-on (NULL: all banks 0, last two PRG fixed)
    void (*state)(struct state_io *io); // the board's own registers (NULL: none; the slots are saved here)
} banked_board_t;

bool mapper_banked_init(ines_data *ines_file_data, mapper_info_t *p_mapper_info, const banked_board_t *board);

void banked_select_prg(unsigned slot, unsigned bank);    // 8KB slot 0-3 ($8000, $A000, $C000, $E000) shows 8KB bank
void banked_select_chr(unsigned slot, unsigned bank);    // 1KB slot 0-7 shows 1KB bank
void banked_select_chr_2k(unsigned slot, unsigned bank); // 1KB slots SLOT (even) and SLOT + 1 show the 2KB bank
void banked_set_mapped(
    unsigned mask); // power_on: which slots (bit n = slot n) have a bank before the first write (default: 2 and 3)
void banked_select_mirroring(bool vertical);
unsigned banked_prg_count(void); // the number of 8KB PRG banks (the fixed banks are banked_prg_count() - 2 and - 1)
unsigned banked_submapper(void); // NES 2.0 submapper, 0 for iNES 1.0

#endif // mapper_banked_h_defined
