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
// SHA-1 (FIPS 180-4): identifies games in the Game Genie code database (No-Intro lists the SHA-1 of the ROM data)
#ifndef sha1_h_defined
#define sha1_h_defined
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[5];
    uint64_t length; // bytes so far
    uint8_t block[64];
    size_t used; // bytes in block
} sha1_ctx;

void sha1_init(sha1_ctx *ctx);
void sha1_update(sha1_ctx *ctx, const uint8_t *data, size_t size);
void sha1_hex(sha1_ctx *ctx, char hex[41]); // finish: 40 upper-case hex digits
#endif
