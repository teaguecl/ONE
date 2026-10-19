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
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "log.h"
#include "cpu.h"
#include "mapper.h"

uint8_t mem_read(uint16_t address)
{
    uint8_t value;

    if (address < 0x2000) {
        value = cpu_read(address & 0x7FFF);
    } else if (address < 0x4000) {
        // ppu stuff
        value = 0; // todo: implement
    } else if (address <= 0x4018) {
        // apu stuff
        value = 0; // todo: implement
    } else {
        // mapper
        value = mapper_read_prg(address);
    }
    return value;
}

void mem_write(uint16_t address, uint8_t value)
{
    LOG_TRACE(CPU, "mem_write %04X = %02X", address, value);
    if (address < 0x2000) {
        cpu_write(address, value);
    } else if (address < 0x4000) {
        // ppu stuff
    } else if (address <= 0x4018) {
        // apu stuff
    } else {
        // mapper
        mapper_write_prg(address, value);
    }
}
