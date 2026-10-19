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

/* VRC6a (iNES mapper 24: Akumajou Densetsu (Castlevania III, Japan)): the VRC6 board (mapper_vrc6.c) with register lines A0 = bit 0, A1 = bit 1. */

#include <stdint.h>
#include "mapper_vrc6a.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_vrc6.h"

bool mapper_vrc6a_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data)
{
    return mapper_vrc6_init(ines_file_data, p_mapper_info, sav_data, false);
}
