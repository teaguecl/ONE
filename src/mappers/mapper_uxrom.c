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

/* UxROM (iNES mapper 2: UNROM, UOROM): 16KB PRG-ROM bank at $8000-$BFFF selected by any write to $8000-$FFFF, the last
 * 16KB fixed at $C000-$FFFF; 8KB CHR-RAM. The board is in mapper_discrete.c. */

#include <stdint.h>
#include "mapper_uxrom.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_discrete.h"

static void uxrom_write_reg(uint8_t value)
{
    discrete_select_prg(value);
}

static const discrete_board_t uxrom_board = {
    .name = "UxROM",
    .prg_bank_size = 0x4000,
    .fixed_last_prg = true,
    .write_reg = uxrom_write_reg,
};

bool mapper_uxrom_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mapper_discrete_init(ines_file_data, p_mapper_info, &uxrom_board);
}
