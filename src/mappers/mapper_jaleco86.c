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

/* Jaleco JF-13 (iNES mapper 86: Moero!! Pro Yakyuu): a write to $6000-$6FFF selects the 32KB PRG-ROM bank (bits 4-5) and the 8KB CHR-ROM bank (bits 0-1 and 6). Writes to $7000-$7FFF drive the speech chip, which ONE does not emulate. Mirroring is fixed by the header, no bus conflicts. The board is in mapper_discrete.c. */

#include <stdint.h>
#include "mapper_jaleco86.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_discrete.h"

static void jaleco86_write_reg(uint16_t address, uint8_t value)
{
    if (address < 0x7000) {
        discrete_select_prg((value >> 4) & 0x03);
        discrete_select_chr((value & 0x03) | ((value >> 4) & 0x04));
    }
}

static const discrete_board_t jaleco86_board = {
    .name = "Jaleco JF-13",
    .prg_bank_size = 0x8000,
    .chr_rom_banked = true,
    .no_bus_conflicts = true,
    .reg_6000 = true,
    .write_reg_at = jaleco86_write_reg,
};

bool mapper_jaleco86_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mapper_discrete_init(ines_file_data, p_mapper_info, &jaleco86_board);
}
