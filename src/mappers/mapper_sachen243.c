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

/* Sachen 74LS374N (iNES mapper 243: Poker II, Honey Peach): $4100 (address & $E101 = $4100) selects one of eight registers, $4101 writes it. Register 5 picks the 32KB PRG-ROM bank (3 bits), registers 2, 4 and 6 the 8KB CHR-ROM bank (bit 0 = bank bit 3, bit 0 = bank bit 2, two bits = bank bits 0-1), register 7 bit 0 the mirroring (1 = vertical). No bus conflicts. The board is in mapper_discrete.c. */

#include <stdint.h>
#include <string.h>
#include "savestate.h"
#include "mapper_sachen243.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_discrete.h"

static uint8_t sachen_index = 0;
static uint8_t sachen_regs[8];

static void sachen243_update(void)
{
    discrete_select_prg(sachen_regs[5] & 0x07);
    discrete_select_chr(((sachen_regs[2] & 1) << 3) | ((sachen_regs[4] & 1) << 2) | (sachen_regs[6] & 0x03));
    discrete_select_mirroring(0 != (sachen_regs[7] & 1));
}

static void sachen243_write_reg(uint16_t address, uint8_t value)
{
    if (0 == (address & 1)) {
        sachen_index = value & 0x07;
    } else {
        sachen_regs[sachen_index] = value & 0x07;
        sachen243_update();
    }
}

static void sachen243_state(state_io *io)
{
    STATE(io, sachen_index);
    STATE(io, sachen_regs);
}

static void sachen243_power_on(void)
{
    sachen_index = 0;
    memset(sachen_regs, 0, sizeof(sachen_regs));
}

static const discrete_board_t sachen243_board = {
    .name = "Sachen 74LS374N",
    .prg_bank_size = 0x8000,
    .chr_rom_banked = true,
    .no_bus_conflicts = true,
    .reg_low = true,
    .power_on = sachen243_power_on,
    .state = sachen243_state,
    .write_reg_at = sachen243_write_reg,
};

bool mapper_sachen243_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mapper_discrete_init(ines_file_data, p_mapper_info, &sachen243_board);
}
