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

/* Irem 74161/32 (iNES mapper 78: Holy Diver, Cosmo Carrier): a write to $8000-$FFFF selects the 16KB PRG-ROM bank at $8000 (bits
 * 0-2, the last 16KB fixed at $C000), the 8KB CHR-ROM bank (bits 4-7) and the mirroring (bit 3), with bus conflicts. Holy Diver
 * (NES 2.0 submapper 3, and the default): 0 = horizontal, 1 = vertical. Cosmo Carrier (submapper 1): one-screen page.
 * The board is in mapper_discrete.c. */

#include <stdint.h>
#include "mapper_irem74161.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_discrete.h"

static void irem74161_write_reg(uint8_t value)
{
    discrete_select_prg(value & 0x07);
    discrete_select_chr(value >> 4);
    if (1 == discrete_submapper()) {
        discrete_select_page((value >> 3) & 1);
    } else {
        discrete_select_mirroring(0 != (value & 0x08));
    }
}

static const discrete_board_t irem74161_board = {
    .name = "Irem 74161",
    .prg_bank_size = 0x4000,
    .fixed_last_prg = true,
    .chr_rom_banked = true,
    .write_reg = irem74161_write_reg,
};

bool mapper_irem74161_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mapper_discrete_init(ines_file_data, p_mapper_info, &irem74161_board);
}
