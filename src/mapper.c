/*
  SPDX-License-Identifier: GPL-2.0-or-later
  ONE NES emulator
  Copyright (C) 1999-2018 Chris Teague

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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "zipfile.h"
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <strings.h> /* strncasecmp */
#endif
#ifdef _MSC_VER
#define strncasecmp _strnicmp /* MSVC spellings of the POSIX functions */
#define fileno      _fileno
#endif
#if !defined(S_ISREG) && defined(S_IFMT) && defined(S_IFREG)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG) /* MSVC's <sys/stat.h> has no S_ISREG */
#endif
#include <assert.h>
#include "log.h"
#include "ines.h"
#include "mappers/mapper_nrom.h"
#include "mappers/mapper_cnrom.h"
#include "mappers/mapper_mmc1.h"
#include "mappers/mapper_txrom.h"
#include "mappers/mapper_exrom.h"
#include "mappers/mapper_pxrom.h"
#include "mappers/mapper_fxrom.h"
#include "mappers/mapper_vrc4bd.h"
#include "mappers/mapper_fme7.h"
#include "mappers/mapper_bandai_fcg.h"
#include "mappers/mapper_nes_event.h"
#include "mappers/mapper_namco163.h"
#include "mappers/mapper_vrc6a.h"
#include "mappers/mapper_vrc6b.h"
#include "mappers/mapper_vrc2b.h"
#include "mappers/mapper_vrc2a.h"
#include "mappers/mapper_vrc4ac.h"
#include "mappers/mapper_uxrom.h"
#include "mappers/mapper_axrom.h"
#include "mappers/mapper_colordreams.h"
#include "mappers/mapper_bnrom.h"
#include "mappers/mapper_gxrom.h"
#include "mappers/mapper_camerica.h"
#include "mappers/mapper_nina003.h"
#include "mappers/mapper_irem74161.h"
#include "mappers/mapper_bandai74161.h"
#include "mappers/mapper_vrc1.h"
#include "mappers/mapper_jaleco86.h"
#include "mappers/mapper_jaleco87.h"
#include "mappers/mapper_nina006.h"
#include "mappers/mapper_sunsoft1.h"
#include "mappers/mapper_cnrom185.h"
#include "mappers/mapper_irem_g101.h"
#include "mappers/mapper_taito_tc0190.h"
#include "mappers/mapper_quattro.h"
#include "mappers/mapper_sachen243.h"
#include "mapper.h"
#include "cheats.h"
#include "crc32.h"
#include "sha1.h"
#include "bits.h"
#include "savestate.h"

static mapper_info_t glb_mapper_info = {0};
bool mapper_watches_a12 = false; // (mapper.h)
bool mapper_watches_ppu = false;
bool mapper_has_exp = false;
bool mapper_ticks = false;

/*******************************************************************************
 * FUNCTION NAME : mapper_sav_filename()
 * ARGUMENTS     : const char *rom_filename - path of the ROM being loaded
 * RETURN        : newly malloc'd path of the matching .sav file (caller frees),
 *                 or NULL if out of memory
 * DESCRIPTION   : "dir/game.nes" -> "dir/game.sav".  Only an extension in the
 *                 last path component is replaced ("dir.v2/game" -> "dir.v2/game.sav").
 *                 Kept separate so the same name can be used when saving.
 *******************************************************************************/
static char *mapper_sav_filename(const char *rom_filename)
{
    size_t len = strlen(rom_filename);
    const char *slash = strrchr(rom_filename, '/');
    const char *dot = strrchr(rom_filename, '.');
    size_t base_len = len;
    char *sav_filename;

    if ((NULL != dot) && ((NULL == slash) || (dot > slash))) {
        base_len = (size_t)(dot - rom_filename); // strip the existing extension
    }
    sav_filename = malloc(base_len + sizeof(".sav"));
    if (NULL == sav_filename) {
        return NULL;
    }
    memcpy(sav_filename, rom_filename, base_len);
    memcpy(sav_filename + base_len, ".sav", sizeof(".sav")); // includes the '\0'
    return sav_filename;
}

/*******************************************************************************
 * FUNCTION NAME : mapper_load_sav_file()
 * ARGUMENTS     : const char *sav_filename - path of the .sav file (from mapper_sav_filename())
 *                 sav_data_t *sav - filled in with the .sav contents
 * RETURN        : true  - the .sav file was loaded, or there is no .sav file
 *                         (sav->size == 0, sav->data == NULL)
 *                 false - a .sav file exists but could not be read; sav is left
 *                         empty.  Treated as fatal by the caller so a later save
 *                         can never overwrite a file we failed to read.
 * DESCRIPTION   : Reads the whole battery-backed RAM file that sits next to the
 *                 ROM.  The mapper decides whether the size fits its PRG-RAM.
 *                 Free the buffer with mapper_free_sav_data().
 *******************************************************************************/
#define SAV_FILE_MAX_SIZE (512 * 1024) // sanity limit; no NES board has more battery RAM

/* Reads the whole of an already-open .sav file into sav.  Returns false (sav left
 * empty) on any error; an empty file is accepted and leaves sav empty.  The caller
 * owns sav_file and closes it, so every failure here can simply return. */
static bool mapper_read_sav_stream(FILE *sav_file, const char *sav_filename, sav_data_t *sav)
{
    struct stat st;
    long file_size;
    size_t bytes_read;

    if ((0 != fstat(fileno(sav_file), &st)) || !S_ISREG(st.st_mode)) {
        LOG_ERROR(MAPPER, "%s is not a regular file", sav_filename);
        return false;
    }
    if (0 != fseek(sav_file, 0, SEEK_END)) {
        LOG_ERROR(MAPPER, "cannot seek in %s: %s", sav_filename, strerror(errno));
        return false;
    }
    file_size = ftell(sav_file);
    if (file_size < 0) {
        LOG_ERROR(MAPPER, "cannot get the size of %s: %s", sav_filename, strerror(errno));
        return false;
    }
    if (0 == file_size) {
        LOG_WARN(MAPPER, "%s is empty, ignoring it", sav_filename);
        return true;
    }
    if (file_size > SAV_FILE_MAX_SIZE) {
        LOG_WARN(MAPPER, "%s is %ld bytes, larger than any cartridge RAM", sav_filename, file_size);
        return false;
    }
    if (0 != fseek(sav_file, 0, SEEK_SET)) {
        LOG_ERROR(MAPPER, "cannot seek in %s: %s", sav_filename, strerror(errno));
        return false;
    }

    sav->data = malloc((size_t)file_size);
    if (NULL == sav->data) {
        LOG_ERROR(MAPPER, "out of memory for %ld bytes of %s", file_size, sav_filename);
        return false;
    }
    bytes_read = fread(sav->data, 1, (size_t)file_size, sav_file);
    if (bytes_read != (size_t)file_size) {
        if (ferror(sav_file)) {
            LOG_ERROR(MAPPER, "error reading %s: %s", sav_filename, strerror(errno));
        } else {
            LOG_ERROR(MAPPER, "%s: read only %zu of %ld bytes", sav_filename, bytes_read, file_size);
        }
        free(sav->data);
        sav->data = NULL;
        return false;
    }
    sav->size = (size_t)file_size;
    LOG_INFO(MAPPER, "loaded %zu bytes from %s", sav->size, sav_filename);
    return true;
}

static bool mapper_load_sav_file(const char *sav_filename, sav_data_t *sav)
{
    FILE *sav_file;
    bool ok;

    assert(NULL != sav_filename);
    assert(NULL != sav);
    sav->size = 0;
    sav->data = NULL;

    sav_file = fopen(sav_filename, "rb"); /* binary: no newline translation */
    if (NULL == sav_file) {
        ok = (ENOENT == errno); /* nothing to load is not an error */
        if (ok) {
            LOG_DEBUG(MAPPER, "no save file (%s)", sav_filename);
        } else {
            LOG_ERROR(MAPPER, "cannot open %s: %s", sav_filename, strerror(errno));
        }
    } else {
        ok = mapper_read_sav_stream(sav_file, sav_filename, sav);
        if (0 != fclose(sav_file)) { /* a read-only close can still report an error */
            LOG_ERROR(MAPPER, "error closing %s: %s", sav_filename, strerror(errno));
        }
    }

    return ok;
}

static void mapper_free_sav_data(sav_data_t *sav)
{
    free(sav->data);
    sav->data = NULL;
    sav->size = 0;
}

/* Flush a file's data to disk (fsync on POSIX, _commit on Windows). Returns 0 on success. */
static int mapper_sync_file(FILE *file)
{
#ifdef _WIN32
    return _commit(_fileno(file));
#else
    return fsync(fileno(file));
#endif
}

/* Rename 'from' to 'to', replacing 'to' if it exists, as one atomic step where the OS allows it.
 * (On Windows rename() refuses to overwrite an existing file.) Returns 0 on success. */
static int mapper_replace_file(const char *from, const char *to)
{
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

/*******************************************************************************
 * FUNCTION NAME : mapper_write_sav_file()
 * ARGUMENTS     : const char *sav_filename - .sav path to create or overwrite
 *                 const uint8_t *data, size_t size - battery RAM contents
 * RETURN        : true if the file was written completely, false otherwise
 * DESCRIPTION   : Writes to "<sav_filename>.tmp", flushes it to disk, then renames
 *                 it over the real file.  rename() is atomic, so a crash or a full
 *                 disk part-way through can never leave a truncated save behind:
 *                 the old .sav stays intact until the new one is complete.
 *******************************************************************************/
static bool mapper_write_sav_file(const char *sav_filename, const uint8_t *data, size_t size)
{
    size_t name_len = strlen(sav_filename);
    char *tmp_filename;
    FILE *sav_file;
    bool ok = true;

    tmp_filename = malloc(name_len + sizeof(".tmp"));
    if (NULL == tmp_filename) {
        LOG_ERROR(MAPPER, "out of memory building the temporary file name");
        return false;
    }
    memcpy(tmp_filename, sav_filename, name_len);
    memcpy(tmp_filename + name_len, ".tmp", sizeof(".tmp")); // includes the '\0'

    sav_file = fopen(tmp_filename, "wb"); /* binary: no newline translation */
    if (NULL == sav_file) {
        LOG_ERROR(MAPPER, "cannot create %s: %s", tmp_filename, strerror(errno));
        free(tmp_filename);
        return false;
    }

    if (fwrite(data, 1, size, sav_file) != size) {
        LOG_ERROR(MAPPER, "error writing %s: %s", tmp_filename, strerror(errno));
        ok = false;
    }
    if (ok && (0 != fflush(sav_file))) {
        LOG_ERROR(MAPPER, "error flushing %s: %s", tmp_filename, strerror(errno));
        ok = false;
    }
    if (ok && (0 != mapper_sync_file(sav_file))) { // make sure the data is on disk before the rename
        LOG_ERROR(MAPPER, "error syncing %s: %s", tmp_filename, strerror(errno));
        ok = false;
    }
    if (0 != fclose(sav_file)) { // close can report a delayed write error
        LOG_ERROR(MAPPER, "error closing %s: %s", tmp_filename, strerror(errno));
        ok = false;
    }
    if (ok && (0 != mapper_replace_file(tmp_filename, sav_filename))) {
        LOG_ERROR(MAPPER, "cannot rename %s to %s: %s", tmp_filename, sav_filename, strerror(errno));
        ok = false;
    }
    if (!ok) {
        remove(tmp_filename); // don't leave a partial file behind
        LOG_ERROR(MAPPER, "%s was NOT updated", sav_filename);
    } else {
        LOG_INFO(MAPPER, "wrote %zu bytes to %s", size, sav_filename);
    }

    free(tmp_filename);
    return ok;
}

/*******************************************************************************
 * FUNCTION NAME : mapper_init()
 * ARGUMENTS     : char *filename - pointer to the file to load.
 * RETURN        : NO_ERROR or ERROR
 * DESCRIPTION   : This function calls ines_load() to read in the iNES format
 *                 file.  Then it determines which mapper is being used, and
 *                 calls the appropriate mapper initialization routine.
 *
 *******************************************************************************/
// Copies of the ROM's PRG and CHR data for scripts (script.c: emu.memType.nesPrgRom / nesChrRom), whatever the mapper
static uint8_t *rom_prg_copy = NULL, *rom_chr_copy = NULL;
static size_t rom_prg_size = 0, rom_chr_size = 0;
static uint32_t rom_crc = 0;   // No-Intro CRC-32: PRG + CHR, without the iNES header (mapper_rom_crc())
static char rom_sha1[41] = ""; // ... and SHA-1 (mapper_rom_sha1()): what the Game Genie code database lists

static void rom_copies_free(void)
{
    free(rom_prg_copy);
    free(rom_chr_copy);
    rom_prg_copy = rom_chr_copy = NULL;
    rom_prg_size = rom_chr_size = 0;
}

static void rom_copies_keep(const ines_data *ines)
{
    rom_copies_free();
    size_t prg = 16384u * ines->header.prg_16kb_banks, chr = 8192u * ines->header.chr_8kb_banks;
    if (ines->prg_rom != NULL && prg > 0) {
        rom_prg_copy = malloc(prg);
        if (rom_prg_copy != NULL) {
            memcpy(rom_prg_copy, ines->prg_rom, prg);
            rom_prg_size = prg;
        }
    }
    if (ines->chr_rom != NULL && chr > 0) {
        rom_chr_copy = malloc(chr);
        if (rom_chr_copy != NULL) {
            memcpy(rom_chr_copy, ines->chr_rom, chr);
            rom_chr_size = chr;
        }
    }
    // the game's identity, as databases know it (No-Intro: the data without the header, which varies between dumps)
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, rom_prg_copy != NULL ? rom_prg_copy : ines->prg_rom, prg);
    if (chr > 0) {
        crc = crc32_update(crc, rom_chr_copy != NULL ? rom_chr_copy : ines->chr_rom, chr);
    }
    rom_crc = crc ^ 0xFFFFFFFFu;
    sha1_ctx sha;
    sha1_init(&sha);
    sha1_update(&sha, rom_prg_copy != NULL ? rom_prg_copy : ines->prg_rom, prg);
    if (chr > 0) {
        sha1_update(&sha, rom_chr_copy != NULL ? rom_chr_copy : ines->chr_rom, chr);
    }
    sha1_hex(&sha, rom_sha1);
}

const char *mapper_rom_sha1(void)
{
    return rom_sha1;
}

uint32_t mapper_rom_crc(void)
{
    return rom_crc;
}

const uint8_t *mapper_debug_prg_rom(size_t *size)
{
    *size = rom_prg_size;
    return rom_prg_copy;
}

const uint8_t *mapper_debug_chr_rom(size_t *size)
{
    *size = rom_chr_size;
    return rom_chr_copy;
}

static bool sav_files = true; // mapper_set_sav_files()

void mapper_set_sav_files(bool on)
{
    sav_files = on;
}

static const char *sav_input = NULL; // mapper_set_sav_input()

void mapper_set_sav_input(const char *path)
{
    sav_input = path;
}

bool mapper_battery_ram(uint8_t **data, size_t *size)
{
    sav_data_t sav = {0, NULL};
    if (!(glb_mapper_info.header.flags_6 & BIT_1) || NULL == glb_mapper_info.get_sav_data) {
        return false;
    }
    glb_mapper_info.get_sav_data(&sav);
    if (sav.size == 0 || sav.data == NULL) {
        return false;
    }
    *data = sav.data;
    *size = sav.size;
    return true;
}

// FILENAME, or a game in memory (MEMORY, MEMORY_SIZE; FILENAME is then only a name)
static bool mapper_init_from(char *filename, const uint8_t *memory, size_t memory_size);

bool mapper_init(char *filename)
{
    if (zip_is_zip_name(filename)) {
        // The first .nes in the ZIP. The battery file is named after that inner file, next to the ZIP
        // ("dir/Game.zip" holding "Game (USA).nes" -> "dir/Game (USA).sav"), as Mesen does.
        uint8_t *data;
        size_t size;
        char entry[512], error[128], path[1024];
        if (!zip_extract_nes(filename, &data, &size, entry, sizeof(entry), error, sizeof(error))) {
            LOG_ERROR(MAPPER, "%s: %s", filename, error);
            return false;
        }
        const char *slash = strrchr(filename, '/');
        int dir_len = slash != NULL ? (int)(slash - filename) + 1 : 0;
        snprintf(path, sizeof(path), "%.*s%s", dir_len, filename, entry);
        bool ok = mapper_init_from(path, data, size);
        free(data);
        return ok;
    }
    return mapper_init_from(filename, NULL, 0);
}

bool mapper_init_memory(const uint8_t *data, size_t size, const char *name)
{
    char copy[1024];
    snprintf(copy, sizeof(copy), "%s", name);
    return mapper_init_from(copy, data, size);
}

static bool mapper_init_from(char *filename, const uint8_t *memory, size_t memory_size)
{
    ines_data ines_file_data;
    char *filename_suffix_ptr;
    sav_data_t sav_file_data = {0, NULL};
    bool status = false;

    assert(NULL != filename);
    LOG_DEBUG(MAPPER, "opening %s", filename);

    size_t filename_len = strlen(filename);
    if (memory == NULL && filename_len < 4) {
        LOG_ERROR(MAPPER, "file name too short: %s", filename);
        return false;
    }

    // work out the .sav path and load the battery-backed RAM file, if there is one
    free(glb_mapper_info.sav_filename);
    glb_mapper_info.sav_filename = NULL;
    if (NULL != sav_input) { // battery RAM from this file, nothing written back
        if (!mapper_load_sav_file(sav_input, &sav_file_data)) {
            return false;
        }
    } else if (sav_files) {
        glb_mapper_info.sav_filename = mapper_sav_filename(filename);
        if (NULL == glb_mapper_info.sav_filename) {
            LOG_ERROR(MAPPER, "out of memory building the .sav file name");
            return false;
        }
        if (!mapper_load_sav_file(glb_mapper_info.sav_filename, &sav_file_data)) {
            free(glb_mapper_info.sav_filename);
            glb_mapper_info.sav_filename = NULL;
            return false;
        }
    }

    filename_suffix_ptr = filename + (filename_len >= 4 ? filename_len - 4 : 0); // todo: find the .
    LOG_DEBUG(MAPPER, "suffix is %s", filename_suffix_ptr);
    if (memory != NULL || strncasecmp(filename_suffix_ptr, ".nes", 4) == 0) {
        uint8_t mapper_num;
        ines_file_data.prg_rom = NULL;
        ines_file_data.chr_rom = NULL;
        if (true == (memory != NULL ? ines_load_memory(memory, memory_size, filename, &ines_file_data)
                                    : ines_load(filename, &ines_file_data))) {
            mapper_num = (ines_file_data.header.flags_6 & 0xF0) >> 4;
            mapper_num |= (ines_file_data.header.flags_7 & 0xF0);

            LOG_DEBUG(MAPPER, "mapper number: %d", mapper_num);
            {
                // start from no hooks: a hook the previous game's mapper set must not outlive it
                char *sav_filename = glb_mapper_info.sav_filename;
                memset(&glb_mapper_info, 0, sizeof(glb_mapper_info));
                glb_mapper_info.sav_filename = sav_filename;
            }
            rom_copies_keep(
                &ines_file_data); // (first: a mapper may need the game's CRC, as the MMC3 does for the MMC6 games)
            if (NROM == mapper_num) {
                status = mapper_nrom_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (SxROM == mapper_num) {
                status = mapper_mmc1_init_ines(&ines_file_data, &glb_mapper_info, &sav_file_data);
            } else if (UxROM == mapper_num) {
                status = mapper_uxrom_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (CNROM == mapper_num) {
                status = mapper_cnrom_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (TxROM == mapper_num || TxSROM == mapper_num || TQROM == mapper_num || Namco108 == mapper_num ||
                       88 == mapper_num || 37 == mapper_num || 47 == mapper_num) {
                status = mapper_txrom_init_ines(&ines_file_data, &glb_mapper_info, &sav_file_data, mapper_num);
            } else if (ExROM == mapper_num) {
                status = mapper_exrom_init_ines(&ines_file_data, &glb_mapper_info, &sav_file_data);
            } else if (FxROM == mapper_num) {
                status = mapper_fxrom_init_ines(&ines_file_data, &glb_mapper_info, &sav_file_data);
            } else if (PxROM == mapper_num) {
                status = mapper_pxrom_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (AxROM == mapper_num) {
                status = mapper_axrom_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (ColorDreams == mapper_num) {
                status = mapper_colordreams_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (BNROM == mapper_num) {
                status = mapper_bnrom_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (GxROM == mapper_num) {
                status = mapper_gxrom_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (Camerica == mapper_num) {
                status = mapper_camerica_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (NINA003 == mapper_num) {
                status = mapper_nina003_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (IremHolyDiver == mapper_num) {
                status = mapper_irem74161_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (Bandai74161 == mapper_num) {
                status = mapper_bandai74161_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (VRC1 == mapper_num) {
                status = mapper_vrc1_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (JalecoJF13 == mapper_num) {
                status = mapper_jaleco86_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (JalecoJF05 == mapper_num) {
                status = mapper_jaleco87_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (NINA006 == mapper_num) {
                status = mapper_nina006_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (Sunsoft1 == mapper_num) {
                status = mapper_sunsoft1_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (CNROM185 == mapper_num) {
                status = mapper_cnrom185_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (IremG101 == mapper_num) {
                status = mapper_irem_g101_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (TaitoTC0190 == mapper_num) {
                status = mapper_taito_tc0190_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (Quattro == mapper_num) {
                status = mapper_quattro_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (Sachen243 == mapper_num) {
                status = mapper_sachen243_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (BandaiFCG == mapper_num) {
                status = mapper_bandai_fcg_init_ines(&ines_file_data, &glb_mapper_info, &sav_file_data);
            } else if (FME7 == mapper_num) {
                status = mapper_fme7_init_ines(&ines_file_data, &glb_mapper_info, &sav_file_data);
            } else if (NESEvent == mapper_num) {
                status = mapper_nes_event_init_ines(&ines_file_data, &glb_mapper_info);
            } else if (Namco163 == mapper_num) {
                status = mapper_namco163_init_ines(&ines_file_data, &glb_mapper_info, &sav_file_data);
            } else if (VRC6a == mapper_num) {
                status = mapper_vrc6a_init_ines(&ines_file_data, &glb_mapper_info, &sav_file_data);
            } else if (VRC6b == mapper_num) {
                status = mapper_vrc6b_init_ines(&ines_file_data, &glb_mapper_info, &sav_file_data);
            } else if (VRC4ac == mapper_num) {
                status = mapper_vrc4ac_init_ines(&ines_file_data, &glb_mapper_info, &sav_file_data);
            } else if (VRC2a == mapper_num) {
                status = mapper_vrc2a_init_ines(&ines_file_data, &glb_mapper_info, &sav_file_data);
            } else if (VRC2b == mapper_num) {
                status = mapper_vrc2b_init_ines(&ines_file_data, &glb_mapper_info, &sav_file_data);
            } else if (VRC4bd == mapper_num) {
                status = mapper_vrc4bd_init_ines(&ines_file_data, &glb_mapper_info, &sav_file_data);
            } else {
                LOG_ERROR(MAPPER, "unsupported mapper: %d", mapper_num);
            }
            glb_mapper_info.header = ines_file_data.header;
            mapper_watches_a12 = status && (NULL != glb_mapper_info.ppu_a12);
            mapper_watches_ppu = status && (NULL != glb_mapper_info.ppu_fetch);
            mapper_has_exp = status && (NULL != glb_mapper_info.read_exp);
            mapper_ticks = status && (NULL != glb_mapper_info.cpu_tick);
            mapper_update_cheats(); // (the new mapper's read_prg: codes on stand in front of it)
            if (!status) {
                rom_copies_free();
            }
            ines_free_data(&ines_file_data);
        }
    } else {
        LOG_ERROR(MAPPER, "unknown file type (not .nes)");
        status = false;
    }
    if (!status) {
        // mapper_deinit() won't be called, so drop the .sav path now
        free(glb_mapper_info.sav_filename);
        glb_mapper_info.sav_filename = NULL;
    }
    mapper_free_sav_data(&sav_file_data); // mappers copy what they need into their own PRG-RAM
    return status;
}

void mapper_deinit(void)
{
    rom_copies_free();
    if ((glb_mapper_info.header.flags_6 & BIT_1) && // battery-backed RAM (the same flag in iNES 1.0 and NES 2.0)
        (NULL != glb_mapper_info.get_sav_data) && (NULL != glb_mapper_info.sav_filename)) {
        sav_data_t sav_data = {0, NULL};
        glb_mapper_info.get_sav_data(&sav_data); // points into the mapper's RAM
        if ((sav_data.size > 0) && (sav_data.data != NULL)) {
            // create or overwrite .sav file (must happen before the mapper frees its RAM)
            (void)mapper_write_sav_file(glb_mapper_info.sav_filename, sav_data.data, sav_data.size);
        }
    }
    free(glb_mapper_info.sav_filename);
    glb_mapper_info.sav_filename = NULL;

    //mapper_nrom_deinit();
    glb_mapper_info.deinit();
}

uint8_t mapper_read_prg(uint16_t address)
{
    return glb_mapper_info.read_prg(address); // (with Game Genie codes on: read_prg_with_cheats())
}

// Game Genie codes (cheats.h) cost nothing while none is on: then glb_mapper_info.read_prg is the mapper's own
// function; with codes on it is read_prg_with_cheats(), which patches what the mapper's function read.
static uint8_t (*own_read_prg)(uint16_t) = NULL; // the mapper's function, while read_prg_with_cheats() stands in

static uint8_t read_prg_with_cheats(uint16_t address)
{
    return cheats_patch(address, own_read_prg(address));
}

void mapper_update_cheats(void)
{
    if (NULL == glb_mapper_info.read_prg) {
        return; // (no game yet: mapper_init() calls this again)
    }
    bool patched = (glb_mapper_info.read_prg == read_prg_with_cheats);
    if (cheats_active && !patched) {
        own_read_prg = glb_mapper_info.read_prg;
        glb_mapper_info.read_prg = read_prg_with_cheats;
    } else if (!cheats_active && patched) {
        glb_mapper_info.read_prg = own_read_prg;
    }
}

void mapper_write_prg(uint16_t address, uint8_t value)
{
    glb_mapper_info.write_prg(address, value);
}

uint8_t mapper_read_chr(uint16_t address)
{
    return glb_mapper_info.read_chr(address);
}

bool mapper_chr_is_open(void)
{
    return (NULL != glb_mapper_info.chr_is_open) && glb_mapper_info.chr_is_open();
}

void mapper_write_chr(uint16_t address, uint8_t value)
{
    glb_mapper_info.write_chr(address, value);
}

// input is the PPU address trying to be accessed
// return value is the ciram address to use
// this is how mirroring is implemented
bool mapper_is_nes2(const ines_header *h)
{
    return (h->flags_7 & 0x0C) == 0x08;
}

// NES 2.0 RAM size field (a nibble of byte 10 or 11): 0 = none, else 64 << n bytes
static uint32_t nes2_ram_size(unsigned shift)
{
    return (0 == shift) ? 0 : (64u << shift);
}

// (ines_header names the NES 2.0 bytes by their iNES 1.0 meaning: byte 8 = prg_ram_8kb_banks, 9 = flags_9,
// 10 = flags_10, 11 = zero_filled[0])
bool mapper_nes2_info(const ines_header *h, nes2_info_t *info)
{
    unsigned mapper = (h->flags_6 >> 4) | (h->flags_7 & 0xF0) | ((h->prg_ram_8kb_banks & 0x0Fu) << 8);
    if (mapper > 255) {
        LOG_ERROR(MAPPER, "NES 2.0 mapper %u is not supported", mapper);
        return false;
    }
    if (0 != h->flags_9) { // ROM sizes past 4MB PRG / 2MB CHR
        LOG_ERROR(MAPPER, "NES 2.0 ROM size (byte 9 = $%02X) is not supported", h->flags_9);
        return false;
    }
    info->submapper = h->prg_ram_8kb_banks >> 4;
    info->prg_ram_size = nes2_ram_size(h->flags_10 & 0x0F);
    info->prg_nvram_size = nes2_ram_size(h->flags_10 >> 4);
    info->chr_ram_size = nes2_ram_size(h->zero_filled[0] & 0x0F);
    LOG_DEBUG(MAPPER, "NES 2.0: mapper %u.%u, PRG-RAM %u + %u (battery) bytes, CHR-RAM %u bytes", mapper,
              info->submapper, info->prg_ram_size, info->prg_nvram_size, info->chr_ram_size);
    return true;
}

// The byte in the ROM (or RAM...) itself, without Game Genie codes: the debugger's 'gg', a code's compare value
uint8_t mapper_debug_read_prg_rom(uint16_t address)
{
    if (NULL != glb_mapper_info.debug_read_prg) {
        return glb_mapper_info.debug_read_prg(address);
    }
    return (glb_mapper_info.read_prg == read_prg_with_cheats) ? own_read_prg(address)
                                                              : glb_mapper_info.read_prg(address);
}

// What the CPU reads there: Game Genie codes included
uint8_t mapper_debug_read_prg(uint16_t address)
{
    uint8_t value = mapper_debug_read_prg_rom(address);
    return cheats_active ? cheats_patch(address, value) : value;
}

uint32_t mapper_a12_rises_to_irq(void)
{
    if (NULL == glb_mapper_info.ppu_a12) {
        return UINT32_MAX;
    }
    return NULL != glb_mapper_info.a12_rises_to_irq ? glb_mapper_info.a12_rises_to_irq() : 1;
}

bool mapper_read_exp(uint16_t address, uint8_t *value)
{
    return (NULL != glb_mapper_info.read_exp) && glb_mapper_info.read_exp(address, value);
}

bool mapper_debug_read_exp(uint16_t address, uint8_t *value)
{
    return (NULL != glb_mapper_info.debug_read_exp) && glb_mapper_info.debug_read_exp(address, value);
}

void mapper_ppu_fetch(uint16_t address, ppu_fetch_kind kind, uint64_t dot)
{
    glb_mapper_info.ppu_fetch(address, kind, dot); // (the callers test mapper_watches_ppu)
}

bool mapper_debug_write_prg(uint16_t address, uint8_t value)
{
    return (NULL != glb_mapper_info.debug_write_prg) && glb_mapper_info.debug_write_prg(address, value);
}

bool mapper_debug_bg_tile(uint16_t nt_address, int32_t *chr_offset, int *palette)
{
    return (NULL != glb_mapper_info.debug_bg_tile) && glb_mapper_info.debug_bg_tile(nt_address, chr_offset, palette);
}

uint32_t mapper_lines_to_irq(void)
{
    return (NULL != glb_mapper_info.lines_to_irq) ? glb_mapper_info.lines_to_irq() : 0; // (0: unknown, may be now)
}

void mapper_cpu_tick(void)
{
    glb_mapper_info.cpu_tick(); // (the caller tests mapper_ticks)
}

void mapper_ppu_ctrl_write(uint8_t value)
{
    if (NULL != glb_mapper_info.ppu_ctrl_write) {
        glb_mapper_info.ppu_ctrl_write(value);
    }
}

uint8_t mapper_read_nt(uint16_t address)
{
    return glb_mapper_info.read_nt(address); // (get_ciram_addr() returned >= $800: the mapper has the hook)
}

void mapper_write_nt(uint16_t address, uint8_t value)
{
    glb_mapper_info.write_nt(address, value);
}

void mapper_ppu_a12(bool high, uint64_t dot)
{
    if (NULL != glb_mapper_info.ppu_a12) {
        glb_mapper_info.ppu_a12(high, dot);
    }
}

uint16_t mapper_get_ciram_addr(uint16_t addr)
{
    return glb_mapper_info.get_ciram_addr(addr);
}

uint8_t *mapper_debug_prg_ram(size_t *size)
{
    *size = 0;
    if (NULL == glb_mapper_info.debug_prg_ram) {
        return NULL;
    }
    return glb_mapper_info.debug_prg_ram(size);
}

int32_t mapper_debug_prg_offset(uint16_t address)
{
    uint32_t size = 16u * 1024u * glb_mapper_info.header.prg_16kb_banks;
    if (address < 0x8000 || size == 0) {
        return -1;
    }
    if (NULL != glb_mapper_info.debug_prg_offset) {
        return glb_mapper_info.debug_prg_offset(address);
    }
    return (int32_t)((address - 0x8000u) % size); // no PRG banking: 16KB is mirrored, 32KB fills $8000-$FFFF
}

bool mapper_debug_prg_bank(uint16_t address, int *bank, int *banks)
{
    uint32_t size = 16u * 1024u * glb_mapper_info.header.prg_16kb_banks;
    int32_t offset = mapper_debug_prg_offset(address);
    if (glb_mapper_info.prg_bank_size == 0 || offset < 0 || size == 0) {
        return false;
    }
    *bank = (int)((uint32_t)offset / glb_mapper_info.prg_bank_size);
    *banks = (int)(size / glb_mapper_info.prg_bank_size);
    return true;
}

bool mapper_debug_prg_banked(void)
{
    return NULL != glb_mapper_info.debug_prg_offset;
}

bool mapper_debug_has_battery(void)
{
    return (glb_mapper_info.header.flags_6 & BIT_1) != 0;
}

uint8_t mapper_debug_read_chr(uint16_t address)
{
    return glb_mapper_info.debug_read_chr(address);
}

int32_t mapper_debug_chr_offset(uint16_t address)
{
    return (NULL != glb_mapper_info.debug_chr_offset) ? glb_mapper_info.debug_chr_offset(address) : -1;
}

const uint8_t *mapper_debug_chr_data(size_t *size)
{
    *size = 0;
    return (NULL != glb_mapper_info.debug_chr_data) ? glb_mapper_info.debug_chr_data(size) : NULL;
}

// Save states: the mapper of the loaded ROM saves its own registers and RAM
void mapper_state(state_io *io)
{
    STATE_SKIP(glb_mapper_info); // the ROM's mapper (functions, header): set by mapper_init()
    STATE_SKIP(mapper_watches_a12);
    STATE_SKIP(mapper_watches_ppu);
    STATE_SKIP(mapper_has_exp);
    STATE_SKIP(mapper_ticks);
    STATE_SKIP(sav_files);    // (libretro: the frontend keeps the battery RAM)
    STATE_SKIP(sav_input);    // (--sav-in)
    STATE_SKIP(own_read_prg); // (a function: set when Game Genie codes go on)
    STATE_SKIP(rom_prg_copy); // copies of the ROM (scripts), set by mapper_init()
    STATE_SKIP(rom_chr_copy);
    STATE_SKIP(rom_prg_size);
    STATE_SKIP(rom_crc);
    STATE_SKIP(rom_sha1);
    STATE_SKIP(rom_chr_size);
    if (glb_mapper_info.state != NULL) {
        glb_mapper_info.state(io);
    }
}
