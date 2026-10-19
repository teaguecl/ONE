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
// CRC-32 (the zlib / PNG / No-Intro one)
#ifndef crc32_h_defined
#define crc32_h_defined
#include <stddef.h>
#include <stdint.h>
// Start with 0xFFFFFFFF, feed the data in any number of pieces, finish with ^ 0xFFFFFFFF
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size);
#endif
