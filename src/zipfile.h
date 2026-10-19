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
// Reading a game out of a .zip file (zipfile.c): the first .nes entry, stored or deflated
#ifndef zipfile_h_defined
#define zipfile_h_defined
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZIP_MAX_ROM_SIZE  (8u * 1024 * 1024) // the largest NES ROM is far below this
#define ZIP_MAX_FILE_SIZE (64u * 1024 * 1024)

// True when NAME ends in ".zip" (any case)
bool zip_is_zip_name(const char *name);

// Unpacks the first .nes entry of the ZIP in MEMORY (SIZE bytes) into a malloc'd buffer (*data, *data_size). The
// entry's name without its directory goes to ENTRY_NAME. Returns false with a message in ERROR (when not NULL).
// Rejects ZIP64, encryption, methods other than stored / deflate, a wrong CRC-32 and anything out of bounds.
bool zip_extract_nes_memory(const uint8_t *memory, size_t size, uint8_t **data, size_t *data_size, char *entry_name,
                            size_t entry_name_size, char *error, size_t error_size);

// The same for the ZIP file at PATH
bool zip_extract_nes(const char *path, uint8_t **data, size_t *data_size, char *entry_name, size_t entry_name_size,
                     char *error, size_t error_size);
#endif
