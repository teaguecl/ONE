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
#ifndef bits_h_defined
#define bits_h_defined
#define BIT_0  (0x01)
#define BIT_1  (0x02)
#define BIT_2  (0x04)
#define BIT_3  (0x08)
#define BIT_4  (0x10)
#define BIT_5  (0x20)
#define BIT_6  (0x40)
#define BIT_7  (0x80)
#define BIT_8  (0x100)
#define BIT_9  (0x200)
#define BIT_10 (0x400)
#define BIT_11 (0x800)
#define BIT_12 (0x1000)
#define BIT_13 (0x2000)
#define BIT_14 (0x4000)
#define BIT_15 (0x8000)

// Index of the lowest set bit of a non-zero 64-bit value (__builtin_ctzll; _BitScanForward64 for MSVC).
#ifdef _MSC_VER
#include <intrin.h>
static inline unsigned bits_ctz64(unsigned long long value)
{
    unsigned long index;
    _BitScanForward64(&index, value);
    return (unsigned)index;
}
#else
static inline unsigned bits_ctz64(unsigned long long value)
{
    return (unsigned)__builtin_ctzll(value);
}
#endif

#endif // bits_h_defined
