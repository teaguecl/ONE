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

/* GxROM (iNES mapper 66, GNROM / MHROM): a write to $8000-$FFFF selects the 32KB PRG-ROM bank (bits 4-5) and the 8KB CHR-ROM
 * bank (bits 0-1), with bus conflicts. Mirroring is fixed by the header. The board is in mapper_discrete.c. */

#include <stdint.h>
#include "mapper_gxrom.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_discrete.h"

static void gxrom_write_reg(uint8_t value)
{
    discrete_select_prg((value >> 4) & 0x03);
    discrete_select_chr(value & 0x03);
}

static const discrete_board_t gxrom_board = {
    .name = "GxROM",
    .prg_bank_size = 0x8000,
    .chr_rom_banked = true,
    .write_reg = gxrom_write_reg,
};

bool mapper_gxrom_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mapper_discrete_init(ines_file_data, p_mapper_info, &gxrom_board);
}
