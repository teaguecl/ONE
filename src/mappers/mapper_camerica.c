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

/* Camerica / Codemasters (iNES mapper 71, BF9093 / BF9097): a write to $C000-$FFFF selects the 16KB PRG-ROM bank at $8000, the
 * last 16KB fixed at $C000; 8KB CHR-RAM. A write to $9000-$9FFF (Fire Hawk's BF9097) bit 4 picks the one-screen page. No bus
 * conflicts. The board is in mapper_discrete.c. */

#include <stdint.h>
#include "mapper_camerica.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_discrete.h"

static void camerica_write_reg(uint16_t address, uint8_t value)
{
    if (address >= 0xC000) {
        discrete_select_prg(value);
    } else if (address >= 0x9000 && address < 0xA000) {
        discrete_select_page((value >> 4) & 1);
    }
}

static const discrete_board_t camerica_board = {
    .name = "Camerica",
    .prg_bank_size = 0x4000,
    .fixed_last_prg = true,
    .no_bus_conflicts = true,
    .write_reg_at = camerica_write_reg,
};

bool mapper_camerica_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mapper_discrete_init(ines_file_data, p_mapper_info, &camerica_board);
}
