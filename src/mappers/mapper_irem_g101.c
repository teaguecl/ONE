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

/* Irem G-101 (iNES mapper 32: Image Fight, Major League, Perman): $8000 = the 8KB PRG bank that is switchable (5 bits), $A000 =
 * the 8KB PRG bank at $A000, $9000 = mirroring (bit 0: 0 = vertical, 1 = horizontal) and PRG mode (bit 1: 0 = the bank from $8000
 * is at $8000 and the second-last 8KB at $C000, 1 = swapped), $B000-$B007 = the eight 1KB CHR banks. The last 8KB is fixed at
 * $E000. The registers decode address & $F000. The board is in mapper_banked.c. */

#include <stdint.h>
#include "mapper_irem_g101.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_banked.h"

#define MAJOR_LEAGUE_CRC 0x243A8735u // No-Intro CRC-32 of Major League (Japan)
#include "savestate.h"

static uint8_t prg_reg = 0;
static bool prg_swapped = false; // $9000 bit 1

static void g101_update_prg(void)
{
    unsigned fixed = banked_prg_count() - 2;
    banked_select_prg(0, prg_swapped ? fixed : prg_reg);
    banked_select_prg(2, prg_swapped ? prg_reg : fixed);
}

static void g101_write_reg(uint16_t address, uint8_t value)
{
    switch (address & 0xF000) {
    case 0x8000:
        prg_reg = value & 0x1F;
        g101_update_prg();
        break;
    case 0x9000:
        banked_select_mirroring(0 == (value & 1));
        prg_swapped = (0 != (value & 2));
        g101_update_prg();
        break;
    case 0xA000: banked_select_prg(1, value & 0x1F); break;
    case 0xB000: banked_select_chr(address & 7, value); break;
    default: break;
    }
}

static void g101_power_on(void)
{
    prg_reg = 0;
    prg_swapped = false;
    // (Mesen: Image Fight has the 8KB at $A000 from the start and waits for the first write to $8000; Major League waits
    // for both, which only its trace shows)
    banked_set_mapped((MAJOR_LEAGUE_CRC == mapper_rom_crc()) ? 0x0C : 0x0E);
}

static void g101_state(state_io *io)
{
    STATE(io, prg_reg);
    STATE(io, prg_swapped);
}

static const banked_board_t g101_board = {
    .name = "Irem G-101",
    .mesen_work_ram = true,
    .write_reg = g101_write_reg,
    .power_on = g101_power_on,
    .state = g101_state,
};

bool mapper_irem_g101_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mapper_banked_init(ines_file_data, p_mapper_info, &g101_board);
}
