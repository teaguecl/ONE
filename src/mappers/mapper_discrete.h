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

/* The hardware that UxROM, CNROM, Color Dreams, BNROM and AxROM share: a board with no RAM-backed registers of its own,
 * just one discrete-logic latch written by any CPU write to $8000-$FFFF (or $4100-$5FFF on NINA-003). The latch picks a PRG-ROM bank, a CHR-ROM bank
 * or the nametable page; the PRG-ROM, which drives the data bus at the same time, ANDs into the written value (bus
 * conflicts). CHR is 8KB CHR-RAM or CHR-ROM; mirroring is fixed by the header or (AxROM) one-screen from the latch.
 * A mapper (mapper_uxrom.c ...) describes its board in a discrete_board_t and decodes the latch in its write_reg. */
#ifndef mapper_discrete_h_defined
#define mapper_discrete_h_defined

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "ines.h"
#include "mapper.h"

struct state_io;

typedef struct {
    const char *name;
    uint32_t prg_bank_size; // 0x4000 or 0x8000: the unit discrete_select_prg() counts in
    bool fixed_last_prg;    // the last 16KB stays at $C000 and the bank register picks the 16KB at $8000 (UxROM)
    bool chr_rom_banked;    // CHR-ROM in 8KB banks, discrete_select_chr() (CNROM, Color Dreams); no CHR-RAM
    bool one_screen;        // nametable page from the latch, discrete_select_page(), not from the header (AxROM)
    bool mesen_work_ram; // MESEN_COMPATIBILITY: 8KB of work RAM at $6000-$7FFF as Mesen has on CNROM (UxROM: open bus)
    bool no_bus_conflicts; // the latch is not ANDed with the ROM (Camerica, NINA-003): the default is a conflict
    bool reg_low; // the latch sits at $4100-$5FFF (address & $E100 == $4100), not at $8000-$FFFF (NINA-003/006)
    bool
        reg_6000; // the latch sits at $6000-$7FFF (Jaleco, Sunsoft), where there is no ROM to conflict with, instead of $8000-$FFFF
    bool chr_4k;   // CHR-ROM in two 4KB banks, discrete_select_chr4(), instead of one 8KB bank (Sunsoft 1)
    bool prg_pair; // the 16KB at $8000 and at $C000 are picked separately, discrete_select_prg_pair() (Camerica 232)
    bool chr_gate; // CHR-ROM can be switched off, discrete_enable_chr() (CNROM copy protection, mapper 185)
    void (*state)(struct state_io *io);                    // the board's own registers, if any (NULL: none)
    void (*power_on)(void);                                // called after the power-on banks are set (NULL: banks 0)
    void (*write_reg)(uint8_t value);                      // the latch was written (after bus conflicts)
    void (*write_reg_at)(uint16_t address, uint8_t value); // instead of write_reg, if the address matters (Camerica)
} discrete_board_t;

bool mapper_discrete_init(ines_data *ines_file_data, mapper_info_t *p_mapper_info, const discrete_board_t *board);

// For the board's write_reg: banks count from 0 in the board's units and wrap around the ROM's size
void discrete_select_prg(unsigned bank);
void discrete_select_chr(unsigned bank);
void discrete_select_chr4(unsigned low, unsigned high);     // 4KB banks at $0000 and $1000 (chr_4k boards)
void discrete_select_prg_pair(unsigned low, unsigned high); // 16KB banks at $8000 and $C000 (prg_pair boards)
void discrete_enable_chr(
    bool on); // chr_gate boards: with CHR off the PPU reads the low address byte (nothing drives the bus)
void discrete_select_page(unsigned page);      // 0 or 1: the CIRAM page all four nametables show (turns one-screen on)
void discrete_select_mirroring(bool vertical); // two-screen mirroring from the latch (turns one-screen off)
void discrete_set_submapper(unsigned submapper); // a board that knows the game (by its CRC) better than the header
unsigned discrete_submapper(void);               // NES 2.0 submapper, 0 for iNES 1.0

#endif // mapper_discrete_h_defined
