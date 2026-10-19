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

/* The shared hardware of the simple 8KB / 1KB register boards: see mapper_banked.h. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "log.h"
#include "cpu.h" // cpu_get_open_bus()
#include "mapper_banked.h"
#include "ines.h"
#include "bits.h"
#include "mapper.h"
#include "savestate.h"

#define CHR_RAM_SIZE (0x2000)

static const banked_board_t *board = NULL;

static uint8_t *prg_rom_storage = NULL;
static uint32_t prg_size_bytes = 0;
static uint32_t prg_bank_count = 0; // 8KB banks
static uint32_t prg_bank[4];        // the 8KB bank in each slot (as written, wrapped)
static uint32_t prg_slot_offset[4]; // byte offset of each
static uint8_t prg_mapped =
    0x0C; // bit n: slot n has been given a bank (the fixed ones from the start); only the Mesen trace shows it

static uint8_t *chr_storage = NULL; // CHR-ROM, or 8KB CHR-RAM if the cartridge has none
static uint32_t chr_size_bytes = 0;
static uint32_t chr_bank_count = 0; // 1KB banks
static uint32_t chr_bank[8];
static uint32_t chr_slot_offset[8];
static bool chr_is_ram = false;

#ifdef MESEN_COMPATIBILITY
#define WORK_RAM_SIZE (0x2000)
static uint8_t work_ram[WORK_RAM_SIZE];
#endif

static bool vertical_mirror = false;
static uint8_t board_submapper = 0;

static void banked_deinit(void);
static uint8_t banked_read_prg(uint16_t address);
static void banked_write_prg(uint16_t address, uint8_t value);
static uint8_t banked_read_chr(uint16_t address);
static void banked_write_chr(uint16_t address, uint8_t value);
static uint16_t banked_get_ciram_addr(uint16_t addr);
static uint8_t banked_debug_read_prg(uint16_t address);
static int32_t banked_debug_prg_offset(uint16_t address);
static int32_t banked_debug_chr_offset(uint16_t address);
static const uint8_t *banked_debug_chr_data(size_t *size);

static void banked_state(state_io *io)
{
    STATE(io, prg_bank);
    STATE(io, prg_mapped);
    STATE(io, chr_bank);
    STATE(io, vertical_mirror);
#ifdef MESEN_COMPATIBILITY
    STATE(io, work_ram);
#endif
    if (chr_is_ram) {
        state_bytes(io, "banked.chr_ram", chr_storage, CHR_RAM_SIZE);
    }
    if (board->state != NULL) {
        board->state(io);
    }
    if (state_loading(io)) { // (also a safe wrap if the state came from another ROM)
        uint8_t mapped = prg_mapped;
        for (unsigned i = 0; i < 4; i++) {
            banked_select_prg(i, prg_bank[i]);
        }
        prg_mapped = mapped;
        for (unsigned i = 0; i < 8; i++) {
            banked_select_chr(i, chr_bank[i]);
        }
    }
    STATE_SKIP(board); // NOLINT(bugprone-sizeof-expression): the macro only names it; the wiring comes from the ROM
    STATE_SKIP(prg_rom_storage);
    STATE_SKIP(prg_size_bytes);
    STATE_SKIP(prg_bank_count);
    STATE_SKIP(prg_slot_offset);
    STATE_SKIP(chr_storage);
    STATE_SKIP(chr_size_bytes);
    STATE_SKIP(chr_bank_count);
    STATE_SKIP(chr_slot_offset);
    STATE_SKIP(chr_is_ram);
    STATE_SKIP(board_submapper);
}

bool mapper_banked_init(ines_data *ines_file_data, mapper_info_t *p_mapper_info, const banked_board_t *b)
{
    assert(ines_file_data != NULL);
    assert(p_mapper_info != NULL);
    assert(b != NULL);
    board = b;
    vertical_mirror = (0 != ines_get_mirroring(ines_file_data));
    board_submapper = 0;
    if ((ines_file_data->header.flags_7 & 0x0C) == 0x08) {
        board_submapper = ines_file_data->header.prg_ram_8kb_banks >> 4; // NES 2.0 byte 8 upper nibble
    }

    prg_size_bytes = 16 * 1024 * ines_file_data->header.prg_16kb_banks;
    if (0 == prg_size_bytes || (prg_size_bytes & 0x3FFF) != 0) {
        LOG_ERROR(MAPPER, "%s: unsupported PRG size", board->name);
        return false;
    }
    prg_rom_storage = malloc(prg_size_bytes);
    if (NULL == prg_rom_storage) {
        LOG_ERROR(MAPPER, "unable to allocate PRG storage");
        return false;
    }
    memcpy(prg_rom_storage, ines_file_data->prg_rom, prg_size_bytes);
    prg_bank_count = prg_size_bytes >> 13;

    chr_bank_count = ines_file_data->header.chr_8kb_banks * 8;
    chr_size_bytes = chr_bank_count * 1024;
    if (0 == chr_bank_count) {
        chr_is_ram = true;
        chr_size_bytes = CHR_RAM_SIZE;
        chr_bank_count = 8;
        chr_storage = (uint8_t *)calloc(1, CHR_RAM_SIZE);
    } else {
        chr_is_ram = false;
        chr_storage = (uint8_t *)malloc(chr_size_bytes);
        if (chr_storage != NULL) {
            memcpy(chr_storage, ines_file_data->chr_rom, chr_size_bytes);
        }
    }
    if (chr_storage == NULL) {
        LOG_ERROR(MAPPER, "Unable to allocate memory for CHR");
        banked_deinit();
        return false;
    }
    for (unsigned i = 0; i < 8; i++) {
        banked_select_chr(i, 0);
    }
    banked_select_prg(0, 0);
    banked_select_prg(1, 0);
    banked_select_prg(2, prg_bank_count - 2);
    banked_select_prg(3, prg_bank_count - 1);
    prg_mapped = 0x0C;
#ifdef MESEN_COMPATIBILITY
    memset(work_ram, 0x00, sizeof(work_ram));
#endif
    if (board->power_on != NULL) {
        board->power_on();
    }

    p_mapper_info->deinit = banked_deinit;
    p_mapper_info->read_prg = banked_read_prg;
    p_mapper_info->write_prg = banked_write_prg;
    p_mapper_info->read_chr = banked_read_chr;
    p_mapper_info->write_chr = banked_write_chr;
    p_mapper_info->get_ciram_addr = banked_get_ciram_addr;
    p_mapper_info->debug_read_chr = banked_read_chr;
    p_mapper_info->debug_read_prg = banked_debug_read_prg;
    p_mapper_info->debug_prg_offset = banked_debug_prg_offset;
    p_mapper_info->debug_chr_offset = banked_debug_chr_offset;
    p_mapper_info->debug_chr_data = banked_debug_chr_data;
    p_mapper_info->prg_bank_size = 0x2000;
    p_mapper_info->state = banked_state;
    return true;
}

static void banked_deinit(void)
{
    LOG_DEBUG(MAPPER, "free'ing PRG and CHR.");
    free(prg_rom_storage);
    prg_rom_storage = NULL;
    free(chr_storage);
    chr_storage = NULL;
}

void banked_select_prg(unsigned slot, unsigned bank)
{
    assert(slot < 4);
    prg_bank[slot] = bank % prg_bank_count;
    prg_slot_offset[slot] = prg_bank[slot] << 13;
    prg_mapped |= (uint8_t)(1u << slot);
}

void banked_set_mapped(unsigned mask)
{
    prg_mapped = (uint8_t)mask;
}

void banked_select_chr(unsigned slot, unsigned bank)
{
    assert(slot < 8);
    chr_bank[slot] = bank % chr_bank_count;
    chr_slot_offset[slot] = chr_bank[slot] << 10;
}

void banked_select_chr_2k(unsigned slot, unsigned bank)
{
    banked_select_chr(slot, bank * 2);
    banked_select_chr(slot + 1, bank * 2 + 1);
}

void banked_select_mirroring(bool vertical)
{
    vertical_mirror = vertical;
}

unsigned banked_prg_count(void)
{
    return prg_bank_count;
}

unsigned banked_submapper(void)
{
    return board_submapper;
}

static uint8_t banked_read_prg(uint16_t address)
{
    if (address < 0x8000) {
#ifdef MESEN_COMPATIBILITY
        if (board->mesen_work_ram) {
            return work_ram[address & (WORK_RAM_SIZE - 1)];
        }
#endif
        return cpu_get_open_bus(); // no PRG-RAM
    }
    return prg_rom_storage[prg_slot_offset[(address >> 13) & 3] + (address & 0x1FFF)];
}

static void banked_write_prg(uint16_t address, uint8_t value)
{
#ifdef MESEN_COMPATIBILITY
    if (address >= 0x6000 && address < 0x8000 && board->mesen_work_ram) {
        work_ram[address & (WORK_RAM_SIZE - 1)] = value;
    }
#endif
    if (address >= 0x8000) {
        board->write_reg(address, value); // (no bus conflicts on these boards: the registers are chips, not a latch)
    }
}

static uint8_t banked_read_chr(uint16_t address)
{
    assert(address < 0x2000);
    return chr_storage[chr_slot_offset[(address >> 10) & 7] + (address & 0x3FF)];
}

static void banked_write_chr(uint16_t address, uint8_t value)
{
    assert(address < 0x2000);
    if (chr_is_ram) {
        chr_storage[chr_slot_offset[(address >> 10) & 7] + (address & 0x3FF)] = value;
    }
}

static uint16_t banked_get_ciram_addr(uint16_t addr)
{
    if (vertical_mirror) {
        return (addr & 0x07FF); // ciram A10 set to PPU A10
    }
    return (addr & 0x03FF) | ((addr & BIT_11) >> 1); // ciram A10 set to PPU A11
}

// Debugger / trace peek: where nothing drives the bus ($6000-$7FFF, and Mesen's slots not yet written) Mesen shows the address high byte
static uint8_t banked_debug_read_prg(uint16_t address)
{
    if (address < 0x8000) {
#ifdef MESEN_COMPATIBILITY
        if (board->mesen_work_ram && address >= 0x6000) {
            return work_ram[address & (WORK_RAM_SIZE - 1)];
        }
#endif
        return (uint8_t)(address >> 8);
    }
#ifdef MESEN_COMPATIBILITY
    if (0 == (prg_mapped & (1u << ((address >> 13) & 3)))) {
        return (uint8_t)(address >> 8);
    }
#endif
    return banked_read_prg(address);
}

static int32_t banked_debug_prg_offset(uint16_t address)
{
    if (address < 0x8000) {
        return -1;
    }
    return (int32_t)(prg_slot_offset[(address >> 13) & 3] + (address & 0x1FFF));
}

static int32_t banked_debug_chr_offset(uint16_t address)
{
    return (int32_t)(chr_slot_offset[(address >> 10) & 7] + (address & 0x3FF));
}

static const uint8_t *banked_debug_chr_data(size_t *size)
{
    *size = chr_size_bytes;
    return chr_storage;
}
