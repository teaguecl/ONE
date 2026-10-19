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

#ifndef mapper_bandai_fcg_h_defined
#define mapper_bandai_fcg_h_defined

#include <stdbool.h>
#include <stdint.h>
#include "ines.h"
#include "mapper.h"

bool mapper_bandai_fcg_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info,
                                 sav_data_t *sav_data); // mapper 16 (Bandai FCG-1/2, LZ93D50 with a 24C02)

#endif // mapper_bandai_fcg_h_defined
