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

/* Taito TC0190 (iNES mapper 33: Bubble Bobble 2, Akira, Don Doko Don): $8000 = 8KB PRG bank at $8000 (bits 0-5) and mirroring
 * (bit 6: 0 = vertical, 1 = horizontal), $8001 = 8KB PRG bank at $A000, $8002 / $8003 = the 2KB CHR banks at $0000 / $0800,
 * $A000-$A003 = the 1KB CHR banks at $1000-$1FFF. The last two 8KB of PRG are fixed. The registers decode address & $A003.
 * No IRQ (that is the TC0690, mapper 48). The board is in mapper_banked.c. */

#include <stdint.h>
#include "mapper_taito_tc0190.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_banked.h"

static void tc0190_write_reg(uint16_t address, uint8_t value)
{
    switch (address & 0xA003) {
    case 0x8000:
        banked_select_prg(0, value & 0x3F);
        banked_select_mirroring(0 == (value & 0x40));
        break;
    case 0x8001: banked_select_prg(1, value & 0x3F); break;
    case 0x8002: banked_select_chr_2k(0, value); break;
    case 0x8003: banked_select_chr_2k(2, value); break;
    case 0xA000:
    case 0xA001:
    case 0xA002:
    case 0xA003: banked_select_chr(4 + (address & 3), value); break;
    default: break;
    }
}

static const banked_board_t tc0190_board = {
    .name = "Taito TC0190",
    .write_reg = tc0190_write_reg,
};

bool mapper_taito_tc0190_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mapper_banked_init(ines_file_data, p_mapper_info, &tc0190_board);
}
