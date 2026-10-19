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
#include "png.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static void crc_init(void)
{
    if (crc_table[1] != 0) {
        return;
    }
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static bool write_chunk(FILE *f, const char *type, const uint8_t *data, size_t size)
{
    uint8_t header[8];
    put32(header, (uint32_t)size);
    memcpy(header + 4, type, 4);
    uint32_t crc = crc_update(0xFFFFFFFFu, header + 4, 4);
    crc = crc_update(crc, data, size) ^ 0xFFFFFFFFu;
    uint8_t trailer[4];
    put32(trailer, crc);
    return fwrite(header, 1, 8, f) == 8 && (size == 0 || fwrite(data, 1, size, f) == size) &&
           fwrite(trailer, 1, 4, f) == 4;
}

bool png_write_rgb(const char *path, const uint8_t *rgb, int width, int height)
{
    crc_init();
    // the raw image: each row is a filter byte (0 = none) and the row's pixels
    size_t row = (size_t)width * 3 + 1;
    size_t raw_size = row * (size_t)height;
    // zlib stream: 2-byte header, stored deflate blocks of up to 65535 bytes (5-byte header each), Adler-32
    size_t blocks = (raw_size + 65534) / 65535;
    size_t zsize = 2 + raw_size + blocks * 5 + 4;
    uint8_t *z = malloc(zsize);
    if (z == NULL) {
        return false;
    }
    uint8_t *out = z;
    *out++ = 0x78; // deflate, 32K window
    *out++ = 0x01; // no preset dictionary, fastest; (0x78 << 8 | 0x01) % 31 == 0
    uint32_t a = 1;
    uint32_t b = 0;
    size_t done = 0;
    for (int y = 0; y < height; y++) {
        for (size_t i = 0; i < row; i++) {
            if (done % 65535 == 0) { // start of a stored block
                size_t left = raw_size - done;
                uint16_t len = (uint16_t)(left < 65535 ? left : 65535);
                *out++ = (left <= 65535) ? 1 : 0; // BFINAL on the last block, BTYPE 00 (stored)
                *out++ = (uint8_t)len;
                *out++ = (uint8_t)(len >> 8);
                uint16_t nlen = (uint16_t)~len;
                *out++ = (uint8_t)nlen;
                *out++ = (uint8_t)(nlen >> 8);
            }
            uint8_t byte = (i == 0) ? 0 : rgb[(size_t)y * (size_t)width * 3 + (i - 1)];
            *out++ = byte;
            a = (a + byte) % 65521;
            b = (b + a) % 65521;
            done++;
        }
    }
    put32(out, (b << 16) | a);
    out += 4;

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        free(z);
        return false;
    }
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)width);
    put32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;  // bits per channel
    ihdr[9] = 2;  // color type: RGB
    ihdr[10] = 0; // deflate
    ihdr[11] = 0; // adaptive filtering (filter type per row)
    ihdr[12] = 0; // not interlaced
    bool ok = fwrite(signature, 1, 8, f) == 8 && write_chunk(f, "IHDR", ihdr, sizeof(ihdr)) &&
              write_chunk(f, "IDAT", z, (size_t)(out - z)) && write_chunk(f, "IEND", NULL, 0);
    free(z);
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        remove(path);
    }
    return ok;
}
