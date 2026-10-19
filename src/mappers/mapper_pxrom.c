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

// Mapper 9: MMC2 (PxROM boards; Mike Tyson's Punch-Out!!). https://www.nesdev.org/wiki/MMC2
//   PRG: an 8KB bank at $8000 ($A000-$AFFF), the last three 8KB banks fixed at $A000-$FFFF. No PRG-RAM.
//   CHR: two 4KB windows, each with two bank registers; a latch per window picks which one. The latches flip when
//   the PPU reads certain pattern bytes (the second plane of tiles $FD / $FE), so a game switches its graphics in
//   the middle of the picture without the CPU (MMC4, mapper 10: the same, but with 16KB PRG banks, 8KB of battery RAM and the left latch
//   firing on every byte of its rows):
//     PPU $0FD8 -> latch 0 = $FD, $0FE8 -> latch 0 = $FE            ($0000 window: $B000 / $C000)
//     PPU $1FD8-$1FDF -> latch 1 = $FD, $1FE8-$1FEF -> latch 1 = $FE ($1000 window: $D000 / $E000)
//   The read that flips a latch still comes from the old bank. $F000 bit 0: mirroring (0 vertical, 1 horizontal).

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "mapper.h"
#include "ines.h"
#include "bits.h"
#include "cpu.h"
#include "savestate.h"

static uint8_t *prg_rom_storage = NULL;
static uint32_t prg_rom_size_bytes = 0;
static uint8_t *chr_rom_storage = NULL;
static uint32_t chr_rom_size_bytes = 0;
static uint32_t prg_slot_offset[4]; // 8KB slots: $8000, $A000, $C000, $E000
static uint32_t chr_slot_offset[2]; // 4KB windows: PPU $0000, $1000

static bool is_mmc4 =
    false; // mapper 10 (FxROM): 16KB PRG banks, work RAM, the left latch fires on any byte of its rows
static uint8_t *wram = NULL;      // MMC4: 8KB at $6000-$7FFF
static bool prg_unmapped = false; // MMC4: $8000-$BFFF are open bus until $A000 is written (Mesen)
static uint8_t prg_bank = 0;      // $A000
static uint8_t chr_bank[2][2];    // [window][latch: 0 = $FD, 1 = $FE] ($B000, $C000, $D000, $E000)
static uint8_t latch[2] = {1, 1}; // per window: 0 = $FD, 1 = $FE
static uint8_t mirroring = 0;     // $F000 bit 0: 0 vertical, 1 horizontal
static uint16_t ciram_page[4];    // CIRAM offset of each name table

static void mapper_pxrom_update_prg_banks(void)
{
    uint32_t banks = prg_rom_size_bytes >> 13; // number of 8KB banks
    if (is_mmc4) {                             // a 16KB bank at $8000, the last 16KB at $C000
        uint32_t b16 = (prg_bank & 0x0Fu) % (banks >> 1);
        prg_slot_offset[0] = (b16 << 14);
        prg_slot_offset[1] = (b16 << 14) + 0x2000;
        prg_slot_offset[2] = (banks - 2) << 13;
        prg_slot_offset[3] = (banks - 1) << 13;
        return;
    }
    prg_slot_offset[0] = ((prg_bank & 0x0Fu) % banks) << 13;
    prg_slot_offset[1] = (banks - 3) << 13;
    prg_slot_offset[2] = (banks - 2) << 13;
    prg_slot_offset[3] = (banks - 1) << 13;
}

// Recomputed when a CHR register or a latch changes, so a CHR read is a single lookup
static void mapper_pxrom_update_chr_banks(void)
{
    uint32_t banks = chr_rom_size_bytes >> 12; // number of 4KB banks
    for (int window = 0; window < 2; window++) {
        chr_slot_offset[window] = ((chr_bank[window][latch[window]] & 0x1Fu) % banks) << 12;
    }
}

static void mapper_pxrom_update_mirroring(void)
{
    unsigned horizontal = mirroring & 1; // vertical: A10 = PPU A10 (pages 0 1 0 1); horizontal: PPU A11 (0 0 1 1)
    for (unsigned nt = 0; nt < 4; nt++) {
        ciram_page[nt] = (uint16_t)(((nt >> horizontal) & 1) << 10);
    }
}

static void mapper_pxrom_get_sav_data(sav_data_t *sav)
{
    sav->data = wram; // still owned by this mapper
    sav->size = 0x2000;
}

static uint8_t *mapper_pxrom_debug_prg_ram(size_t *size)
{
    *size = 0x2000;
    return wram;
}

static void mapper_pxrom_deinit(void)
{
    free(prg_rom_storage);
    prg_rom_storage = NULL;
    prg_rom_size_bytes = 0;
    free(chr_rom_storage);
    chr_rom_storage = NULL;
    chr_rom_size_bytes = 0;
    free(wram);
    wram = NULL;
    is_mmc4 = false;
    prg_unmapped = false;
}

static uint8_t mapper_pxrom_read_prg(uint16_t address)
{
    if (address >= 0x8000) {
        if (prg_unmapped && (address < 0xC000)) {
            return cpu_get_open_bus();
        }
        return prg_rom_storage[prg_slot_offset[(address >> 13) & 3] | (address & 0x1FFF)];
    }
    if (is_mmc4 && (address >= 0x6000)) {
        return wram[address & 0x1FFF];
    }
    return cpu_get_open_bus(); // no PRG-RAM
}

static void mapper_pxrom_write_prg(uint16_t address, uint8_t value)
{
    if (is_mmc4 && (address >= 0x6000) && (address < 0x8000)) {
        wram[address & 0x1FFF] = value;
        return;
    }
    switch (address & 0xF000) {
    case 0xA000:
        prg_bank = value;
        prg_unmapped = false;
        mapper_pxrom_update_prg_banks();
        break;
    case 0xB000: chr_bank[0][0] = value; break;
    case 0xC000: chr_bank[0][1] = value; break;
    case 0xD000: chr_bank[1][0] = value; break;
    case 0xE000: chr_bank[1][1] = value; break;
    case 0xF000:
        mirroring = value & 1;
        mapper_pxrom_update_mirroring();
        return;
    default: return; // ($6000-$9FFF: nothing)
    }
    mapper_pxrom_update_chr_banks();
}

static uint8_t mapper_pxrom_read_chr(uint16_t address)
{
    assert(address < 0x2000);
    uint8_t value = chr_rom_storage[chr_slot_offset[(address >> 12) & 1] | (address & 0x0FFF)];
    unsigned row = address & 0x0FF8; // the 8 second-plane bytes of a tile: $xFD8-$xFDF or $xFE8-$xFEF flip a latch
    if ((row == 0x0FD8) || (row == 0x0FE8)) {
        unsigned window = (address >> 12) & 1;
        if ((window == 1) || is_mmc4 ||
            ((address & 7) == 0)) { // (MMC2 latch 0: only the row's first byte, $0FD8 / $0FE8)
            uint8_t new_latch = (row == 0x0FE8) ? 1 : 0;
            if (latch[window] != new_latch) {
                latch[window] = new_latch;
                mapper_pxrom_update_chr_banks();
            }
        }
    }
    return value;
}

static void mapper_pxrom_write_chr(uint16_t address, uint8_t value)
{
    (void)address; // CHR-ROM
    (void)value;
}

static uint16_t mapper_pxrom_get_ciram_addr(uint16_t addr)
{
    return ciram_page[(addr >> 10) & 3] | (addr & 0x3FF);
}

// The debugger's reads must not flip the latches
static uint8_t mapper_pxrom_debug_read_chr(uint16_t address)
{
    return chr_rom_storage[chr_slot_offset[(address >> 12) & 1] | (address & 0x0FFF)];
}

static int32_t mapper_pxrom_debug_chr_offset(uint16_t address)
{
    return (int32_t)(chr_slot_offset[(address >> 12) & 1] | (address & 0x0FFF));
}

static const uint8_t *mapper_pxrom_debug_chr_data(size_t *size)
{
    *size = chr_rom_size_bytes;
    return chr_rom_storage;
}

static int32_t mapper_pxrom_debug_prg_offset(uint16_t address)
{
    return (int32_t)(prg_slot_offset[(address >> 13) & 3] | (address & 0x1FFF));
}

// Debugger/trace peek: no PRG-RAM shows as the address high byte (as Mesen's debugger, and cpu_debug_read())
static uint8_t mapper_pxrom_debug_read_prg(uint16_t address)
{
    if (is_mmc4 && (address >= 0x6000) && (address < 0x8000)) {
        return wram[address & 0x1FFF];
    }
    if (prg_unmapped && (address >= 0x8000) && (address < 0xC000)) {
        return (uint8_t)(address >> 8);
    }
    return (address < 0x8000) ? (uint8_t)(address >> 8) : mapper_pxrom_read_prg(address);
}

// Save states: the registers and latches, and the offsets they select
static void mapper_pxrom_state(state_io *io)
{
    if (is_mmc4) {
        state_bytes(io, "mmc4.wram", wram, 0x2000);
    }
    STATE_SKIP(wram);
    STATE_SKIP(is_mmc4);
    STATE(io, prg_unmapped);
    STATE(io, prg_bank);
    STATE(io, chr_bank);
    STATE(io, latch);
    STATE(io, mirroring);
    STATE(io, prg_slot_offset);
    STATE(io, chr_slot_offset);
    STATE(io, ciram_page);
    STATE_SKIP(prg_rom_storage); // (the pointers and sizes come from the ROM)
    STATE_SKIP(prg_rom_size_bytes);
    STATE_SKIP(chr_rom_storage);
    STATE_SKIP(chr_rom_size_bytes);
}

static bool mmc24_init(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data, bool mmc4);

bool mapper_pxrom_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    return mmc24_init(ines_file_data, p_mapper_info, NULL, false);
}

bool mapper_pxrom_init_mmc4(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data)
{
    return mmc24_init(ines_file_data, p_mapper_info, sav_data, true);
}

static bool mmc24_init(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data, bool mmc4)
{
    is_mmc4 = mmc4;
    assert(ines_file_data != NULL);
    assert(p_mapper_info != NULL);
    const ines_header *h = &ines_file_data->header;

    if (mapper_is_nes2(h)) {
        nes2_info_t nes2;
        if (!mapper_nes2_info(h, &nes2)) {
            return false;
        }
        if ((0 != nes2.prg_ram_size) || (0 != nes2.prg_nvram_size)) {
            LOG_WARN(MAPPER, "MMC2 with PRG-RAM (PlayChoice-10) is not supported: no PRG-RAM");
        }
    }
    // the fixed banks need at least 32KB of PRG (four 8KB banks), CHR must be ROM
    if ((h->prg_16kb_banks < 2) || (0 == h->chr_8kb_banks)) {
        LOG_ERROR(MAPPER, "MMC2 needs at least 32KB of PRG-ROM and CHR-ROM");
        return false;
    }
    prg_rom_size_bytes = 16 * 1024 * h->prg_16kb_banks;
    chr_rom_size_bytes = 8 * 1024 * h->chr_8kb_banks;
    prg_rom_storage = malloc(prg_rom_size_bytes);
    chr_rom_storage = malloc(chr_rom_size_bytes);
    if ((NULL == prg_rom_storage) || (NULL == chr_rom_storage)) {
        LOG_ERROR(MAPPER, "unable to allocate PRG/CHR storage");
        mapper_pxrom_deinit();
        return false;
    }
    memcpy(prg_rom_storage, ines_file_data->prg_rom, prg_rom_size_bytes);
    memcpy(chr_rom_storage, ines_file_data->chr_rom, chr_rom_size_bytes);

    // power-on state (Mesen's): registers 0, both latches $FE; mirroring from the header until $F000 is written
    prg_bank = 0;
    memset(chr_bank, 0, sizeof(chr_bank));
    latch[0] = latch[1] = 1;
    mirroring = (h->flags_6 & BIT_0) ? 0 : 1;
    if (is_mmc4) {
        wram = calloc(1, 0x2000);
        if (NULL == wram) {
            mapper_pxrom_deinit();
            return false;
        }
        if ((h->flags_6 & BIT_1) && (NULL != sav_data) && (sav_data->size > 0)) {
            memcpy(wram, sav_data->data, (sav_data->size < 0x2000) ? sav_data->size : 0x2000);
        }
#ifdef MESEN_COMPATIBILITY
        prg_unmapped = true;
#endif
        p_mapper_info->get_sav_data = mapper_pxrom_get_sav_data;
        p_mapper_info->debug_prg_ram = mapper_pxrom_debug_prg_ram;
    }
    mapper_pxrom_update_prg_banks();
    mapper_pxrom_update_chr_banks();
    mapper_pxrom_update_mirroring();

    p_mapper_info->deinit = mapper_pxrom_deinit;
    p_mapper_info->read_prg = mapper_pxrom_read_prg;
    p_mapper_info->write_prg = mapper_pxrom_write_prg;
    p_mapper_info->read_chr = mapper_pxrom_read_chr;
    p_mapper_info->write_chr = mapper_pxrom_write_chr;
    p_mapper_info->get_ciram_addr = mapper_pxrom_get_ciram_addr;
    p_mapper_info->debug_read_chr = mapper_pxrom_debug_read_chr;
    p_mapper_info->debug_chr_offset = mapper_pxrom_debug_chr_offset;
    p_mapper_info->debug_chr_data = mapper_pxrom_debug_chr_data;
    p_mapper_info->debug_prg_offset = mapper_pxrom_debug_prg_offset;
    p_mapper_info->prg_bank_size = is_mmc4 ? 0x4000 : 0x2000;
    p_mapper_info->debug_read_prg = mapper_pxrom_debug_read_prg;
    p_mapper_info->state = mapper_pxrom_state;
    return true;
}
