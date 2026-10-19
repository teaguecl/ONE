/*
  SPDX-License-Identifier: GPL-2.0-or-later
  ONE NES emulator
  Copyright (C) 1999-2009 Chris Teague

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

/********************************* REVISIONS ***********************************
 * 09-07-2009   CLT   File created
 *******************************************************************************/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "log.h"
#include "bits.h"
#include "ines.h"


/*******************************************************************************
 * FUNCTION NAME : ines_load()
 * ARGUMENTS     : char *filename - pointer to the filename to load
 *               : ines_data *ines_file_data - pointer to the structure to fill
 *                 with data from the file
 * RETURN        : false on error, true on success.
 * DESCRIPTION   : This function opens the file given in the first parameter and
 *                 reads the PRG and CHR data out of it.  It will allocate space
 *                 for the data in the ines_file_data structure using malloc() and
 *                 copy the data from the file into the structure.
 *******************************************************************************/
// Where the bytes come from: a file, or memory (the libretro core; fmemopen() does not exist on Windows)
typedef struct {
    FILE *file; // NULL: memory
    const uint8_t *data;
    size_t size;
    size_t pos;
} ines_stream;

static size_t stream_read(ines_stream *s, void *buffer, size_t count)
{
    if (s->file != NULL) {
        return fread(buffer, 1, count, s->file);
    }
    size_t left = s->size - s->pos;
    count = count < left ? count : left;
    memcpy(buffer, s->data + s->pos, count);
    s->pos += count;
    return count;
}

static void stream_close(ines_stream *s)
{
    if (s->file != NULL) {
        fclose(s->file);
    }
}

static bool ines_read(ines_stream *ines_file, const char *filename, ines_data *ines_file_data);

bool ines_load(char *filename, ines_data *ines_file_data)
{
    LOG_DEBUG(MAPPER, "loading %s", filename);
    ines_stream stream = {fopen(filename, "rb"), NULL, 0, 0}; /* binary: no newline / EOF translation on Windows */
    if (stream.file == NULL) {
        LOG_ERROR(MAPPER, "can't open %s", filename);
        return false;
    }
    return ines_read(&stream, filename, ines_file_data); // (closes it)
}

// The same from a file already in memory (the libretro core: the frontend hands over the game, perhaps out of a ZIP).
// NAME is for messages.
bool ines_load_memory(const uint8_t *data, size_t size, const char *name, ines_data *ines_file_data)
{
    ines_stream stream = {NULL, data, size, 0};
    return ines_read(&stream, name, ines_file_data);
}

// Reads the file from its stream and closes the stream
static bool ines_read(ines_stream *ines_file, const char *filename, ines_data *ines_file_data)
{
    const uint8_t INES_ID[4] = {'N', 'E', 'S', 0x1A};
    uint32_t prg_rom_size_bytes = 0;
    uint32_t chr_rom_size_bytes = 0;

    /* read the header in from file */
    if (16 != stream_read(ines_file, &ines_file_data->header, 16)) {
        LOG_ERROR(MAPPER, "%s is too short to be an iNES file.", filename);
        stream_close(ines_file);
        return false;
    }
    /* at this point some Endiannes conversion may be necessary */

    if (memcmp(ines_file_data->header.identification, INES_ID, 4) != 0) {
        LOG_ERROR(MAPPER, "%s is an unknown file type.", filename);
        stream_close(ines_file);
        return false;
    }

    // NES 2.0 is not decoded here: the header bytes are passed on as they are, and a mapper that supports NES 2.0
    // reads its fields itself (mapper_txrom.c); the others treat the file as iNES 1.0
    bool NES20_format = ((ines_file_data->header.flags_7 & 0x0C) == 0x08);
    LOG_DEBUG(MAPPER, "iNES format version: %s", NES20_format ? "2.0" : "1.0");

    // A "dirty" iNES 1.0 header: bytes 12-15 are not zero because a tool wrote its name over bytes 7-15 ("DiskDude!",
    // "blargg"). Those bytes are then not header fields -- the mapper's upper nibble included -- so they are ignored,
    // as the nesdev wiki recommends and Mesen does.
    const uint8_t *tail = &ines_file_data->header.zero_filled[1]; // (bytes 12-15)
    if (!NES20_format && (tail[0] | tail[1] | tail[2] | tail[3]) != 0) {
        LOG_INFO(MAPPER, "%s: iNES header bytes 7-15 hold junk (not zero): ignored", filename);
        ines_file_data->header.flags_7 = 0;
        ines_file_data->header.prg_ram_8kb_banks = 0;
        ines_file_data->header.flags_9 = 0;
        ines_file_data->header.flags_10 = 0;
        memset(ines_file_data->header.zero_filled, 0, sizeof(ines_file_data->header.zero_filled));
    }


    prg_rom_size_bytes = 16 * 1024 * ines_file_data->header.prg_16kb_banks;
    chr_rom_size_bytes = 8 * 1024 * ines_file_data->header.chr_8kb_banks;
    LOG_DEBUG(MAPPER, "prg size:%u = %X (%d 16k banks)", prg_rom_size_bytes, prg_rom_size_bytes,
              ines_file_data->header.prg_16kb_banks);
    LOG_DEBUG(MAPPER, "chr size:%u = %X (%d 8k banks)", chr_rom_size_bytes, chr_rom_size_bytes,
              ines_file_data->header.chr_8kb_banks);

    LOG_DEBUG(MAPPER, "flags_6: 0x%02X", ines_file_data->header.flags_6);
    LOG_DEBUG(MAPPER, "flags_7: 0x%02X", ines_file_data->header.flags_7);
    LOG_DEBUG(MAPPER, "prg_ram field: %d", ines_file_data->header.prg_ram_8kb_banks);

    assert(ines_file_data->prg_rom == NULL);
    ines_file_data->prg_rom = (uint8_t *)malloc(prg_rom_size_bytes);
    if (ines_file_data->prg_rom == NULL) {
        LOG_ERROR(MAPPER, "Unable to allocate %u bytes of memory.", prg_rom_size_bytes);
        stream_close(ines_file);
        return false;
    }
    if (prg_rom_size_bytes != stream_read(ines_file, ines_file_data->prg_rom, prg_rom_size_bytes)) {
        LOG_ERROR(MAPPER, "Error reading PRG data");
        stream_close(ines_file);
        free(ines_file_data->prg_rom);
        ines_file_data->prg_rom = NULL;
        return false;
    }

    assert(ines_file_data->chr_rom == NULL);
    ines_file_data->chr_rom = (uint8_t *)malloc(chr_rom_size_bytes);
    if (ines_file_data->chr_rom == NULL) {
        LOG_ERROR(MAPPER, "Unable to allocate %u bytes of memory.", prg_rom_size_bytes);
        stream_close(ines_file);
        free(ines_file_data->prg_rom);
        ines_file_data->prg_rom = NULL;
        return false;
    }
    if (chr_rom_size_bytes != stream_read(ines_file, ines_file_data->chr_rom, chr_rom_size_bytes)) {
        LOG_ERROR(MAPPER, "Error reading CHR data");
        stream_close(ines_file);
        free(ines_file_data->prg_rom);
        ines_file_data->prg_rom = NULL;
        free(ines_file_data->chr_rom);
        ines_file_data->chr_rom = NULL;
        return false;
    }

    stream_close(ines_file);
    LOG_DEBUG(MAPPER, "done loading");
    return true;
}

/*******************************************************************************
 * FUNCTION NAME : unif_free_data()
 * ARGUMENTS     : unif_data *unif_file_data - pointer to the structure to free
 * RETURN        : void
 * DESCRIPTION   : This function frees all of the malloc'd memory areas in the
 *                 structure passed as a parameter.  It is important than all
 *                 unused elements in the ->chunks[] array be set to NULL before
 *                 calling this function, or a segmentation fault will occur.
 *******************************************************************************/
void ines_free_data(ines_data *ines_file_data)
{
    LOG_DEBUG(MAPPER, "attempting to free ines_data");
    assert(ines_file_data != NULL);
    assert(ines_file_data->prg_rom != NULL);
    assert(ines_file_data->chr_rom != NULL);
    free(ines_file_data->prg_rom);
    ines_file_data->prg_rom = NULL;
    free(ines_file_data->chr_rom);
    ines_file_data->chr_rom = NULL;
    LOG_DEBUG(MAPPER, "Done freeing ines_data");
}

uint8_t ines_get_mirroring(ines_data *ines_file_data)
{
    assert(NULL != ines_file_data);
    return (ines_file_data->header.flags_6 & BIT_0);
}
