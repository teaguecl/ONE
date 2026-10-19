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

/* Jaleco / Konami (iNES mapper 87: Goonies, Choplifter): a write to $6000-$7FFF selects the 8KB CHR-ROM bank with its two low bits swapped (bit 0 is the high bit of the bank, bit 1 the low). PRG-ROM is not banked, mirroring is fixed by the header, no bus conflicts. The board is in mapper_discrete.c. */

#include <stdint.h>
#include "mapper_jaleco87.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_discrete.h"

static void jaleco87_write_reg(uint16_t address, uint8_t value)
{
    (void)address;
    discrete_select_chr(((value & 1) << 1) | ((value >> 1) & 1));
}

static const discrete_board_t jaleco87_board = {
    .name = "Jaleco 87",
    .prg_bank_size = 0x8000,
    .chr_rom_banked = true,
    .no_bus_conflicts = true,
    .reg_6000 = true,
    .write_reg_at = jaleco87_write_reg,
};

bool mapper_jaleco87_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mapper_discrete_init(ines_file_data, p_mapper_info, &jaleco87_board);
}
