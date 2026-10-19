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

/* VRC4b / VRC4d / VRC2c (iNES mapper 25: Gradius II, Parodius Da!): the register lines are A1 (bit 0) and A0 (bit 1) (VRC4b, VRC2c), or
 * A3 and A2 (VRC4d); without a NES 2.0 submapper both are decoded, as a VRC4. */

#include <stdint.h>
#include "mapper_vrc4bd.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_vrc24.h"

static const vrc24_board_t vrc4_b_or_d = {"VRC4b/d", 0x000A, 0x0005, true, false};
static const vrc24_board_t vrc4b = {"VRC4b", 0x0002, 0x0001, true, false};
static const vrc24_board_t vrc4d = {"VRC4d", 0x0008, 0x0004, true, false};
static const vrc24_board_t vrc2c = {"VRC2c", 0x0002, 0x0001, false, false};

bool mapper_vrc4bd_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data)
{
    const vrc24_board_t *board = &vrc4_b_or_d;
    switch (vrc24_submapper(&ines_file_data->header)) {
    case 1: board = &vrc4b; break;
    case 2: board = &vrc4d; break;
    case 3: board = &vrc2c; break;
    default: break;
    }
    return mapper_vrc24_init(ines_file_data, p_mapper_info, sav_data, board);
}
