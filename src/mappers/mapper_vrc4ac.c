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

/* VRC4a / VRC4c (iNES mapper 21: Wai Wai World 2, Ganbare Goemon Gaiden 2): the register lines are A1/A2 (VRC4a) or A6/A7
 * (VRC4c); without a NES 2.0 submapper both are decoded. */

#include <stdint.h>
#include "mapper_vrc4ac.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_vrc24.h"

static const vrc24_board_t vrc4_a_or_c = {"VRC4a/c", 0x0042, 0x0084, true, false};
static const vrc24_board_t vrc4a = {"VRC4a", 0x0002, 0x0004, true, false};
static const vrc24_board_t vrc4c = {"VRC4c", 0x0040, 0x0080, true, false};

bool mapper_vrc4ac_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data)
{
    const vrc24_board_t *board = &vrc4_a_or_c;
    switch (vrc24_submapper(&ines_file_data->header)) {
    case 1: board = &vrc4a; break;
    case 2: board = &vrc4c; break;
    default: break;
    }
    return mapper_vrc24_init(ines_file_data, p_mapper_info, sav_data, board);
}
