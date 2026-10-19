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

/* Konami VRC1 (iNES mapper 75: Ganbare Goemon!, Tetsuwan Atom, King Kong 2): $8000 / $A000 / $C000 = the 8KB PRG banks at $8000 /
 * $A000 / $C000 (4 bits), the last 8KB fixed at $E000; $9000 = mirroring (bit 0: 0 = vertical, 1 = horizontal) and the high bit
 * of the two 4KB CHR banks (bits 1 and 2); $E000 / $F000 = the low 4 bits of the 4KB CHR banks at $0000 / $1000. The registers
 * decode address & $F000. The board is in mapper_banked.c. */

#include <stdint.h>
#include "mapper_vrc1.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_banked.h"
#include "savestate.h"

static uint8_t chr_low[2];
static uint8_t chr_high[2];

static void vrc1_update_chr(unsigned half)
{
    unsigned bank = (unsigned)(chr_low[half] | (chr_high[half] << 4)) * 4;
    for (unsigned i = 0; i < 4; i++) {
        banked_select_chr(half * 4 + i, bank + i);
    }
}

static void vrc1_write_reg(uint16_t address, uint8_t value)
{
    switch (address & 0xF000) {
    case 0x8000: banked_select_prg(0, value & 0x0F); break;
    case 0x9000:
        banked_select_mirroring(0 == (value & 1));
        chr_high[0] = (value >> 1) & 1;
        chr_high[1] = (value >> 2) & 1;
        vrc1_update_chr(0);
        vrc1_update_chr(1);
        break;
    case 0xA000: banked_select_prg(1, value & 0x0F); break;
    case 0xC000: banked_select_prg(2, value & 0x0F); break;
    case 0xE000:
        chr_low[0] = value & 0x0F;
        vrc1_update_chr(0);
        break;
    case 0xF000:
        chr_low[1] = value & 0x0F;
        vrc1_update_chr(1);
        break;
    default: break;
    }
}

static void vrc1_power_on(void)
{
    chr_low[0] = chr_low[1] = chr_high[0] = chr_high[1] = 0;
    banked_set_mapped(0x08); // only the last 8KB is fixed
}

static void vrc1_state(state_io *io)
{
    STATE(io, chr_low);
    STATE(io, chr_high);
}

static const banked_board_t vrc1_board = {
    .name = "VRC1",
    .write_reg = vrc1_write_reg,
    .power_on = vrc1_power_on,
    .state = vrc1_state,
};

bool mapper_vrc1_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mapper_banked_init(ines_file_data, p_mapper_info, &vrc1_board);
}
