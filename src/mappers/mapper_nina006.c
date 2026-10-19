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

/* NINA-03 / NINA-06 as used by the Australian multicarts (iNES mapper 113): a write to $4100-$5FFF (address & $E100 = $4100) selects the 32KB PRG-ROM bank (bits 3-5), the 8KB CHR-ROM bank (bits 0-2 and 6) and the mirroring (bit 7: 1 = vertical). No bus conflicts. The board is in mapper_discrete.c. */

#include <stdint.h>
#include "mapper_nina006.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_discrete.h"

static void nina006_write_reg(uint8_t value)
{
    discrete_select_prg((value >> 3) & 0x07);
    discrete_select_chr((value & 0x07) | ((value >> 3) & 0x08));
    discrete_select_mirroring(0 != (value & 0x80));
}

static const discrete_board_t nina006_board = {
    .name = "NINA-06",
    .prg_bank_size = 0x8000,
    .chr_rom_banked = true,
    .no_bus_conflicts = true,
    .reg_low = true,
    .write_reg = nina006_write_reg,
};

bool mapper_nina006_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mapper_discrete_init(ines_file_data, p_mapper_info, &nina006_board);
}
