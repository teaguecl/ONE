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

/* BNROM (iNES mapper 34): a write to $8000-$FFFF selects the 32KB PRG-ROM bank; 8KB CHR-RAM. The board is in
 * mapper_discrete.c. Mapper 34 is also NINA-001 (CHR-ROM, registers at $7FFD-$7FFF, PRG-RAM): mapper_nina001.c. */

#include <stdint.h>
#include "mapper_bnrom.h"
#include "mapper_nina001.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_discrete.h"

static void bnrom_write_reg(uint8_t value)
{
    discrete_select_prg(value);
}

static const discrete_board_t bnrom_board = {
    .name = "BNROM",
    .prg_bank_size = 0x8000,
    .write_reg = bnrom_write_reg,
};

bool mapper_bnrom_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    if (ines_file_data->header.chr_8kb_banks != 0) {
        return mapper_nina001_init_ines(ines_file_data, p_mapper_info); // CHR-ROM: the NINA-001 board
    }
    return mapper_discrete_init(ines_file_data, p_mapper_info, &bnrom_board);
}
