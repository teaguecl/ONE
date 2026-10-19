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

/* Sunsoft 1 (iNES mapper 184: Atlantis no Nazo, Wing of Madoola): a write to $6000-$7FFF selects the two 4KB CHR-ROM banks: bits 0-2 the one at $0000, bits 4-6 the one at $1000 (taken from the upper half of the CHR-ROM). PRG-ROM is 32KB, not banked; mirroring is fixed by the header; no bus conflicts. The board is in mapper_discrete.c. */

#include <stdint.h>
#include "mapper_sunsoft1.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_discrete.h"

static void sunsoft1_write_reg(uint16_t address, uint8_t value)
{
    (void)address;
    discrete_select_chr4(value & 0x07, 0x04 | ((value >> 4) & 0x07));
}

static const discrete_board_t sunsoft1_board = {
    .name = "Sunsoft 1",
    .prg_bank_size = 0x8000,
    .chr_rom_banked = true,
    .chr_4k = true,
    .no_bus_conflicts = true,
    .reg_6000 = true,
    .write_reg_at = sunsoft1_write_reg,
};

bool mapper_sunsoft1_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mapper_discrete_init(ines_file_data, p_mapper_info, &sunsoft1_board);
}
