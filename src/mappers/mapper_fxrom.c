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

/* MMC4 (iNES mapper 10, FxROM: Fire Emblem, Fire Emblem Gaiden, Famicom Wars): the MMC2's CHR latches (the left one fires on every byte of its rows),
 * a 16KB PRG bank at $8000 ($A000), the last 16KB at $C000, 8KB of battery RAM at $6000. The chip is in mapper_pxrom.c. */

#include <stdbool.h>
#include "ines.h"
#include "mapper.h"
#include "mapper_fxrom.h"
#include "mapper_pxrom.h"

bool mapper_fxrom_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data)
{
    return mapper_pxrom_init_mmc4(ines_file_data, p_mapper_info, sav_data);
}
