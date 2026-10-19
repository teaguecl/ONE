/*
  SPDX-License-Identifier: GPL-2.0-or-later
  ONE NES emulator
  Copyright (C) 1999-2017 Chris Teague

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

#ifndef mapper_nrom_h_defined
#define mapper_nrom_h_defined

#include "ines.h"
#include "mapper.h"

bool mapper_nrom_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info);
uint8_t mapper_nrom_read_byte_lower_prg(uint16_t address);
uint8_t mapper_nrom_read_byte_upper_prg(uint16_t address);
void mapper_nrom_write_byte_lower_prg(uint16_t address, uint8_t value);
void mapper_nrom_write_byte_upper_prg(uint16_t address, uint8_t value);
uint8_t mapper_nrom_read_byte_chr(uint16_t address);
void mapper_nrom_write_byte_chr(uint16_t address, uint8_t value);
void mapper_nrom_deinit(void);
uint8_t mapper_nrom_read_prg(uint16_t address);
void mapper_nrom_write_prg(uint16_t address, uint8_t value);
uint8_t mapper_nrom_read_chr(uint16_t address);
void mapper_nrom_write_chr(uint16_t address, uint8_t value);
uint16_t mapper_nrom_get_ciram_addr(uint16_t addr);

uint8_t mapper_nrom_debug_read_chr(uint16_t address);
#endif
