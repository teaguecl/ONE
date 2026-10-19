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

/* The hardware of Konami's VRC6 (iNES mappers 24 and 26): a 16KB and an 8KB PRG bank, eight CHR registers with three banking modes,
 * mirroring, a VRC4-style IRQ and expansion audio (two pulse channels and a sawtooth). VRC6a (24) and VRC6b (26) differ in which
 * of the CPU's A0 / A1 select a register in a group of four: a mapper (mapper_vrc6a.c, mapper_vrc6b.c) swaps them or not.
 * https://www.nesdev.org/wiki/VRC6 */
#ifndef mapper_vrc6_h_defined
#define mapper_vrc6_h_defined

#include <stdbool.h>
#include <stdint.h>
#include "ines.h"
#include "mapper.h"

bool mapper_vrc6_init(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data, bool swap_a0_a1);

#endif // mapper_vrc6_h_defined
