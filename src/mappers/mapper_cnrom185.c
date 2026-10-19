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

/* CNROM with the copy-protection of iNES mapper 185 (Seicross, Bird Week, Mighty Bomb Jack): a write to $8000-$FFFF switches the 8KB CHR-ROM on or off (it is only one bank); when off nothing drives the PPU data bus. NES 2.0 submappers 4-7 want the written value & 3 to be 0-3; without a submapper the games are told apart as the common emulators do: CHR is on if the low four bits are not 0 and the value is not $13. With bus conflicts. Mirroring is fixed by the header. The board is in mapper_discrete.c. */

#include <stdint.h>
#include "mapper_cnrom185.h"
#include "ines.h"
#include "mapper.h"
#include "mapper_discrete.h"

#define SEICROSS_CRC 0x0F05FF0Au // No-Intro CRC-32 of Seicross (Japan)

static void cnrom185_write_reg(uint8_t value)
{
    unsigned submapper = discrete_submapper();
    if (submapper >= 4) {
        discrete_enable_chr((value & 0x03) == (submapper - 4));
    } else {
        discrete_enable_chr((value & 0x0F) != 0 && value != 0x13);
    }
}

static const discrete_board_t cnrom185_board = {
    .name = "CNROM 185",
    .prg_bank_size = 0x8000,
    .chr_rom_banked = true,
    .chr_gate = true,
    .write_reg = cnrom185_write_reg,
};

bool mapper_cnrom185_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    if (!mapper_discrete_init(ines_file_data, p_mapper_info, &cnrom185_board)) {
        return false;
    }
    if (0 == discrete_submapper() && SEICROSS_CRC == mapper_rom_crc()) {
        discrete_set_submapper(4); // (an iNES 1.0 file: Seicross tries $21, then $20, and CHR must be off for $21)
    }
    return true;
}
