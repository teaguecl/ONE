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

/* CNROM (iNES mapper 3): fixed 16/32KB PRG-ROM, one 8KB CHR-ROM bank selected by any write to $8000-$FFFF. No PRG-RAM.
 * The board is in mapper_discrete.c. */

#include <stdint.h>
#include "mapper_cnrom.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_discrete.h"

static void cnrom_write_reg(uint8_t value)
{
    discrete_select_chr(value);
}

static const discrete_board_t cnrom_board = {
    .name = "CNROM",
    .prg_bank_size = 0x8000,
    .chr_rom_banked = true,
    .mesen_work_ram = true,
    .write_reg = cnrom_write_reg,
};

bool mapper_cnrom_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mapper_discrete_init(ines_file_data, p_mapper_info, &cnrom_board);
}
