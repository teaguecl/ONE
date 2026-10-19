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
#ifndef zapper_h_defined
#define zapper_h_defined

#include <stdint.h>

// The Zapper light gun on port 2 ($4017 bit 3: light sense, 0 = light; bit 4: trigger, 1 = pulled).
// The model is fitted to measurements against Mesen 2.2.1 (tests/zapper).
#define ZAPPER_BITS 0x18u

// Bits 3 and 4 of a $4017 read at the PPU's present position. Only called when input_zapper_present().
uint8_t zapper_read(void);

#endif
