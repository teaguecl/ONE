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
// Games out of .zip files (zipfile.h; ctest zip_games). Fixtures made once with Python's zipfile (tests/zip/):
// deflate (dynamic Huffman) and stored nestest, a tiny deflate (fixed Huffman) ROM, a ZIP without a .nes.
//   zip_test NESTEST_ROM ZIP_DIR
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zipfile.h"

static int failures = 0;

static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(*size + 1);
    if (data == NULL || fread(data, 1, *size, f) != *size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    return data;
}

static int extract_equals(const char *zip, const uint8_t *want, size_t want_size, const char *want_entry)
{
    uint8_t *data;
    size_t size;
    char entry[64], error[128];
    if (!zip_extract_nes(zip, &data, &size, entry, sizeof(entry), error, sizeof(error))) {
        printf("     %s: %s\n", zip, error);
        return 0;
    }
    int ok = size == want_size && memcmp(data, want, size) == 0 && strcmp(entry, want_entry) == 0;
    free(data);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: zip_test NESTEST_ROM ZIP_DIR\n");
        return 2;
    }
    size_t rom_size, zip_size;
    uint8_t *rom = slurp(argv[1], &rom_size);
    char path[1024];
    if (rom == NULL) {
        fprintf(stderr, "no %s\n", argv[1]);
        return 2;
    }
    check("zip_is_zip_name", zip_is_zip_name("a/b.ZIP") && zip_is_zip_name("x.zip") && !zip_is_zip_name("x.nes") &&
                                 !zip_is_zip_name("zip"));

    snprintf(path, sizeof(path), "%s/nestest_deflate.zip", argv[2]);
    check("deflate (dynamic Huffman), the entry in a directory, debris before it",
          extract_equals(path, rom, rom_size, "NESTEST.NES"));
    snprintf(path, sizeof(path), "%s/nestest_stored.zip", argv[2]);
    check("stored", extract_equals(path, rom, rom_size, "nestest.nes"));

    uint8_t tiny[56] = {'N', 'E', 'S', 0x1A};
    for (int i = 0; i < 12; i++) {
        tiny[4 + i] = (uint8_t)i;
    }
    memset(tiny + 16, 'A', 40);
    snprintf(path, sizeof(path), "%s/tiny.zip", argv[2]);
    check("deflate (fixed Huffman)", extract_equals(path, tiny, sizeof(tiny), "tiny.nes"));

    uint8_t *data;
    size_t size;
    char error[128];
    snprintf(path, sizeof(path), "%s/no_rom.zip", argv[2]);
    check("no .nes in the ZIP is refused", !zip_extract_nes(path, &data, &size, NULL, 0, error, sizeof(error)) &&
                                               strstr(error, "no .nes") != NULL && data == NULL);
    check("a file that is not there", !zip_extract_nes("no/such.zip", &data, &size, NULL, 0, error, sizeof(error)));
    check("not a ZIP", !zip_extract_nes_memory(rom, rom_size, &data, &size, NULL, 0, error, sizeof(error)));

    // damage: every truncation, and every byte flipped, of the small ZIP and of the head of the big one. The reader
    // must fail or return, never crash (ASan) -- and never hand out bytes that are not the ROM (CRC-32).
    snprintf(path, sizeof(path), "%s/tiny.zip", argv[2]);
    uint8_t *zip = slurp(path, &zip_size);
    int bad_accepted = 0, truncated_ok = 0;
    for (size_t n = 0; n < zip_size; n++) {
        if (zip_extract_nes_memory(zip, n, &data, &size, NULL, 0, NULL, 0)) {
            truncated_ok++;
            free(data);
        }
    }
    check("every truncation of a ZIP is refused", truncated_ok == 0);
    for (size_t i = 0; i < zip_size; i++) {
        for (int bit = 0; bit < 8; bit++) {
            zip[i] ^= (uint8_t)(1 << bit);
            if (zip_extract_nes_memory(zip, zip_size, &data, &size, NULL, 0, NULL, 0)) {
                if (size != sizeof(tiny) || memcmp(data, tiny, size) != 0) {
                    bad_accepted++;
                }
                free(data);
            }
            zip[i] ^= (uint8_t)(1 << bit);
        }
    }
    check("flipped bits: failed cleanly, or still the same ROM", bad_accepted == 0);
    free(zip);

    snprintf(path, sizeof(path), "%s/nestest_deflate.zip", argv[2]);
    zip = slurp(path, &zip_size);
    int big_bad = 0;
    for (size_t i = 0; i < zip_size; i += 7) { // (the whole ZIP, every 7th byte: inflate's dynamic tables, the CRC)
        zip[i] ^= 0x10;
        if (zip_extract_nes_memory(zip, zip_size, &data, &size, NULL, 0, NULL, 0)) {
            if (size != rom_size || memcmp(data, rom, size) != 0) {
                big_bad++;
            }
            free(data);
        }
        zip[i] ^= 0x10;
    }
    check("flipped bytes of the big ZIP", big_bad == 0);
    free(zip);
    free(rom);

    if (failures != 0) {
        printf("zip_test: %d FAILED\n", failures);
        return 1;
    }
    printf("zip_test: all passed\n");
    return 0;
}
