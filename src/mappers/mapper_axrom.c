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

/* AxROM (iNES mapper 7: AMROM, ANROM, AOROM): a write to $8000-$FFFF selects the 32KB PRG-ROM bank (bits 0-2) and which
 * CIRAM page all four nametables show (bit 4); 8KB CHR-RAM. The board is in mapper_discrete.c. */

#include <stdint.h>
#include "mapper_axrom.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_discrete.h"

static void axrom_write_reg(uint8_t value)
{
    discrete_select_prg(value & 0x07);
    discrete_select_page((value >> 4) & 1);
}

static const discrete_board_t axrom_board = {
    .name = "AxROM",
    .prg_bank_size = 0x8000,
    .one_screen = true,
    .write_reg = axrom_write_reg,
};

bool mapper_axrom_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mapper_discrete_init(ines_file_data, p_mapper_info, &axrom_board);
}
