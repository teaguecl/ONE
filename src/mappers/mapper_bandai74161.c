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

/* Bandai 74161/161/32 (iNES mapper 70: Kamen Rider Club, Family Trainer 5 and 6): a write to $8000-$FFFF selects the 16KB PRG-ROM bank at $8000 (bits 4-6, the last 16KB fixed at $C000) and the 8KB CHR-ROM bank (bits 0-3), with bus conflicts. Mirroring is fixed by the header. The board is in mapper_discrete.c. */

#include <stdint.h>
#include "mapper_bandai74161.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_discrete.h"

static void bandai74161_write_reg(uint8_t value)
{
    discrete_select_prg((value >> 4) & 0x07);
    discrete_select_chr(value & 0x0F);
}

static const discrete_board_t bandai74161_board = {
    .name = "Bandai 74161",
    .prg_bank_size = 0x4000,
    .fixed_last_prg = true,
    .chr_rom_banked = true,
    .write_reg = bandai74161_write_reg,
};

bool mapper_bandai74161_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mapper_discrete_init(ines_file_data, p_mapper_info, &bandai74161_board);
}
