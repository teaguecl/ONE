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

#ifndef mapper_txrom_h_defined
#define mapper_txrom_h_defined

#include "ines.h"
#include "mapper.h"
#include "settings.h"

const setting_group *mapper_txrom_settings(void); // mapper.mmc3_irq (settings.c lists it)
bool mapper_txrom_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data,
                            unsigned mapper_number); // mapper 4 (MMC3, MMC6), 118 (TxSROM), 119 (TQROM)

#endif // mapper_txrom_h_defined
