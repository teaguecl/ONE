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

/* Camerica Quattro (iNES mapper 232: Quattro Adventure / Arcade / Sports): the 256KB PRG-ROM is four 64KB games. A write to
 * $8000-$BFFF selects the game (bits 3-4), a write to $C000-$FFFF the 16KB page inside it (bits 0-1) at $8000; the game's last
 * page is fixed at $C000. 8KB CHR-RAM, mirroring from the header, no bus conflicts. The board is in mapper_discrete.c. */

#include <stdint.h>
#include <string.h>
#include "mapper_quattro.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_discrete.h"
#include "savestate.h"

static uint8_t outer = 0; // the game
static uint8_t inner = 0; // the page

static void quattro_update(void)
{
    discrete_select_prg_pair(outer * 4u + inner, outer * 4u + 3u);
}

static void quattro_write_reg(uint16_t address, uint8_t value)
{
    if (address < 0xC000) {
        outer = (value >> 3) & 3;
    } else {
        inner = value & 3;
    }
    quattro_update();
}

static void quattro_power_on(void)
{
    outer = inner = 0;
    quattro_update();
}

static void quattro_state(state_io *io)
{
    STATE(io, outer);
    STATE(io, inner);
}

static const discrete_board_t quattro_board = {
    .name = "Camerica Quattro",
    .prg_bank_size = 0x4000,
    .prg_pair = true,
    .no_bus_conflicts = true,
    .power_on = quattro_power_on,
    .state = quattro_state,
    .write_reg_at = quattro_write_reg,
};

bool mapper_quattro_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mapper_discrete_init(ines_file_data, p_mapper_info, &quattro_board);
}
