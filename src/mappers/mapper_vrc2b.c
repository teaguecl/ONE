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

/* VRC2b / VRC4e / VRC4f (iNES mapper 23: Crisis Force, Contra (J)): the register lines are A0/A1 (VRC2b, VRC4f) or A2/A3 (VRC4e);
 * without a NES 2.0 submapper both are decoded, as a VRC4. */

#include <stdint.h>
#include "mapper_vrc2b.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_vrc24.h"

static const vrc24_board_t vrc4_e_or_f = {"VRC4e/f", 0x0005, 0x000A, true, false};
static const vrc24_board_t vrc4f = {"VRC4f", 0x0001, 0x0002, true, false};
static const vrc24_board_t vrc4e = {"VRC4e", 0x0004, 0x0008, true, false};
static const vrc24_board_t vrc2b = {"VRC2b", 0x0001, 0x0002, false, false};

bool mapper_vrc2b_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data)
{
    const vrc24_board_t *board = &vrc4_e_or_f;
    switch (vrc24_submapper(&ines_file_data->header)) {
    case 1: board = &vrc4f; break;
    case 2: board = &vrc4e; break;
    case 3: board = &vrc2b; break;
    default: break;
    }
    return mapper_vrc24_init(ines_file_data, p_mapper_info, sav_data, board);
}
