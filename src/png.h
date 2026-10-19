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
// A minimal PNG writer (screenshots): 8-bit RGB, no compression library needed (the image data is stored in
// uncompressed deflate blocks, so a 256x240 screenshot is about 185 KB; any PNG reader opens it).
#ifndef png_h_defined
#define png_h_defined

#include <stdbool.h>
#include <stdint.h>

// RGB: width x height pixels, 3 bytes each (R, G, B), row by row. Returns false if PATH cannot be written.
bool png_write_rgb(const char *path, const uint8_t *rgb, int width, int height);

#endif
