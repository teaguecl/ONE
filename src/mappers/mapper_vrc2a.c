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

/* VRC2a (iNES mapper 22: TwinBee 3, Ganbare Goemon Gaiden): the register lines are A1 (bit 0) and A0 (bit 1), and the CHR bank number
 * is shifted right by one. */

#include <stdint.h>
#include "mapper_vrc2a.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_vrc24.h"

static const vrc24_board_t vrc2a = {"VRC2a", 0x0002, 0x0001, false, true};

bool mapper_vrc2a_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data)
{
    const vrc24_board_t *board = &vrc2a;
    return mapper_vrc24_init(ines_file_data, p_mapper_info, sav_data, board);
}
