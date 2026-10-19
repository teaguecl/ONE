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

/* AVE NINA-001 (iNES mapper 34 with CHR-ROM: Impossible Mission II, Deadly Towers). https://www.nesdev.org/wiki/NINA-001
 *   8KB of PRG-RAM at $6000-$7FFF whose last three bytes are also registers:
 *     $7FFD  the 32KB PRG-ROM bank at $8000
 *     $7FFE  the 4KB CHR-ROM bank at PPU $0000
 *     $7FFF  the 4KB CHR-ROM bank at PPU $1000
 *   A write there stores into the RAM as well. Mirroring is fixed by the header. */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "bits.h"
#include "cpu.h"
#include "ines.h"
#include "log.h"
#include "mapper.h"
#include "mapper_nina001.h"
#include "savestate.h"

#define WRAM_SIZE 0x2000u

static uint8_t *prg_rom_storage = NULL;
static uint32_t prg_rom_size_bytes = 0;
static uint8_t *chr_storage = NULL;
static uint32_t chr_size_bytes = 0;
static uint8_t *wram = NULL;
static uint8_t prg_reg;
static uint8_t chr_reg[2];
static uint32_t prg_offset;
static uint32_t chr_slot_offset[2]; // 4KB windows
static uint16_t ciram_page[4];

static void nina001_update(void)
{
    prg_offset = ((prg_reg % (prg_rom_size_bytes >> 15)) << 15);
    for (unsigned i = 0; i < 2; i++) {
        chr_slot_offset[i] = (chr_reg[i] % (chr_size_bytes >> 12)) << 12;
    }
}

static void nina001_deinit(void)
{
    free(prg_rom_storage);
    prg_rom_storage = NULL;
    prg_rom_size_bytes = 0;
    free(chr_storage);
    chr_storage = NULL;
    chr_size_bytes = 0;
    free(wram);
    wram = NULL;
}

static uint8_t nina001_read_prg(uint16_t address)
{
    if (address >= 0x8000) {
        return prg_rom_storage[prg_offset | (address & 0x7FFF)];
    }
    if (address >= 0x6000) {
        return wram[address & (WRAM_SIZE - 1)];
    }
    return cpu_get_open_bus();
}

static void nina001_write_prg(uint16_t address, uint8_t value)
{
    if ((address < 0x6000) || (address >= 0x8000)) {
        return;
    }
    wram[address & (WRAM_SIZE - 1)] = value;
    switch (address) {
    case 0x7FFD: prg_reg = value; break;
    case 0x7FFE: chr_reg[0] = value; break;
    case 0x7FFF: chr_reg[1] = value; break;
    default: return;
    }
    nina001_update();
}

static uint8_t nina001_read_chr(uint16_t address)
{
    assert(address < 0x2000);
    return chr_storage[chr_slot_offset[(address >> 12) & 1] | (address & 0x0FFF)];
}

static void nina001_write_chr(uint16_t address, uint8_t value)
{
    (void)address; // CHR-ROM
    (void)value;
}

static uint16_t nina001_get_ciram_addr(uint16_t addr)
{
    return ciram_page[(addr >> 10) & 3] | (addr & 0x3FF);
}

static int32_t nina001_debug_chr_offset(uint16_t address)
{
    return (int32_t)(chr_slot_offset[(address >> 12) & 1] | (address & 0x0FFF));
}

static const uint8_t *nina001_debug_chr_data(size_t *size)
{
    *size = chr_size_bytes;
    return chr_storage;
}

static int32_t nina001_debug_prg_offset(uint16_t address)
{
    return (int32_t)(prg_offset | (address & 0x7FFF));
}

static uint8_t *nina001_debug_prg_ram(size_t *size)
{
    *size = WRAM_SIZE;
    return wram;
}

static uint8_t nina001_debug_read_prg(uint16_t address)
{
    return (address < 0x6000) ? (uint8_t)(address >> 8) : nina001_read_prg(address);
}

static void nina001_state(state_io *io)
{
    STATE(io, prg_reg);
    STATE(io, chr_reg);
    STATE(io, prg_offset);
    STATE(io, chr_slot_offset);
    STATE(io, ciram_page);
    state_bytes(io, "nina001.wram", wram, WRAM_SIZE);
    STATE_SKIP(prg_rom_storage); // (the pointers and sizes come from the ROM)
    STATE_SKIP(prg_rom_size_bytes);
    STATE_SKIP(chr_storage);
    STATE_SKIP(chr_size_bytes);
    STATE_SKIP(wram);
}

bool mapper_nina001_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    assert(ines_file_data != NULL);
    assert(p_mapper_info != NULL);
    const ines_header *h = &ines_file_data->header;
    if ((h->prg_16kb_banks < 2) || (0 == h->chr_8kb_banks)) {
        LOG_ERROR(MAPPER, "NINA-001 needs 32KB of PRG-ROM and CHR-ROM");
        return false;
    }
    prg_rom_size_bytes = 16 * 1024 * h->prg_16kb_banks;
    chr_size_bytes = 8 * 1024 * h->chr_8kb_banks;
    prg_rom_storage = malloc(prg_rom_size_bytes);
    chr_storage = malloc(chr_size_bytes);
    wram = calloc(1, WRAM_SIZE);
    if ((NULL == prg_rom_storage) || (NULL == chr_storage) || (NULL == wram)) {
        LOG_ERROR(MAPPER, "unable to allocate PRG/CHR storage");
        nina001_deinit();
        return false;
    }
    memcpy(prg_rom_storage, ines_file_data->prg_rom, prg_rom_size_bytes);
    memcpy(chr_storage, ines_file_data->chr_rom, chr_size_bytes);

    prg_reg = 0;
    chr_reg[0] = 0;
    chr_reg[1] = 1;
    nina001_update();
    unsigned horizontal = (h->flags_6 & BIT_0) ? 0 : 1; // vertical: pages 0 1 0 1; horizontal: 0 0 1 1
    for (unsigned nt = 0; nt < 4; nt++) {
        ciram_page[nt] = (uint16_t)(((nt >> horizontal) & 1) << 10);
    }

    p_mapper_info->deinit = nina001_deinit;
    p_mapper_info->read_prg = nina001_read_prg;
    p_mapper_info->write_prg = nina001_write_prg;
    p_mapper_info->read_chr = nina001_read_chr;
    p_mapper_info->write_chr = nina001_write_chr;
    p_mapper_info->get_ciram_addr = nina001_get_ciram_addr;
    p_mapper_info->debug_read_chr = nina001_read_chr;
    p_mapper_info->debug_chr_offset = nina001_debug_chr_offset;
    p_mapper_info->debug_chr_data = nina001_debug_chr_data;
    p_mapper_info->debug_prg_offset = nina001_debug_prg_offset;
    p_mapper_info->debug_prg_ram = nina001_debug_prg_ram;
    p_mapper_info->prg_bank_size = 0x8000;
    p_mapper_info->debug_read_prg = nina001_debug_read_prg;
    p_mapper_info->state = nina001_state;
    return true;
}
