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
#ifndef memory_h_defined
#define memory_h_defined

#include <stdint.h>

uint8_t mem_read(uint16_t address);
void mem_write(uint16_t address, uint8_t value);
#ifdef DEBUGGING_ENABLED
uint8_t mem_debug_read(uint16_t address);
#endif // DEBUGGING_ENABLED

#endif
