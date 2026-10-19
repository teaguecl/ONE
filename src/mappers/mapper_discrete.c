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

/* The shared hardware of the one-latch boards: see mapper_discrete.h. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "log.h"
#include "cpu.h" // cpu_get_open_bus()
#include "mapper_discrete.h"
#include "ines.h"
#include "bits.h"
#include "mapper.h"
#include "savestate.h"

#define CHR_RAM_SIZE (0x2000)

static const discrete_board_t *board = NULL;

static uint8_t *prg_rom_storage = NULL;
static uint32_t prg_size_bytes = 0;
static uint32_t prg_bank_count = 0; // in units of the board's PRG bank size (at least 1)
static uint32_t prg_bank = 0;       // the register: the bank at $8000 (UxROM: 16KB; the others: 32KB)
static uint32_t prg_offset[2];      // byte offset of the ROM data at $8000 and at $C000 (16KB each)

static uint8_t *chr_storage = NULL; // CHR-ROM, or CHR-RAM if the cartridge has no CHR-ROM
static uint32_t chr_size_bytes = 0;
static uint32_t chr_bank_count = 0; // 8KB banks
static uint32_t chr_bank = 0;       // the register: the 8KB CHR-ROM bank (chr_4k boards: the 4KB bank at $0000)
static uint32_t chr_bank_high = 0;  // chr_4k boards: the 4KB bank at $1000
static uint32_t prg_bank_high = 0;  // prg_pair boards: the 16KB bank at $C000
static bool chr_enabled = true;     // chr_gate boards
static uint32_t chr_offset[2];      // byte offset of the CHR-ROM at $0000 and at $1000 (4KB each)
static bool chr_is_ram = false;

static bool vertical_mirror = false; // iNES flags 6 bit 0 set: CIRAM A10 = PPU A10
static bool one_screen = false;     // all four name tables show nametable_page (always on AxROM; Camerica when written)
static uint32_t nametable_page = 0; // one-screen: the page the latch picked
static uint8_t board_submapper = 0;
static bool bus_conflicts = true;

#ifdef MESEN_COMPATIBILITY
// Mesen gives a CNROM cart 8KB of work RAM at $6000-$7FFF (discrete_board_t::mesen_work_ram), but not UxROM (the
// Castlevania trace reads open bus there). Real boards have none (reads are open bus): the default build.
#define WORK_RAM_SIZE (0x2000)
static uint8_t work_ram[WORK_RAM_SIZE];
#endif

static void discrete_deinit(void);
static uint8_t discrete_read_prg(uint16_t address);
static void discrete_write_prg(uint16_t address, uint8_t value);
static uint8_t discrete_read_chr(uint16_t address);
static bool discrete_chr_is_open(void);
static void discrete_write_chr(uint16_t address, uint8_t value);
static uint16_t discrete_get_ciram_addr(uint16_t addr);
static uint8_t discrete_debug_read_prg(uint16_t address);
static int32_t discrete_debug_prg_offset(uint16_t address);
static int32_t discrete_debug_chr_offset(uint16_t address);
static const uint8_t *discrete_debug_chr_data(size_t *size);

// Save states: the three registers (the offsets follow from them), CHR-RAM and the work RAM
static void discrete_state(state_io *io)
{
    STATE(io, prg_bank);
    STATE(io, chr_bank);
    STATE(io, chr_bank_high);
    STATE(io, prg_bank_high);
    STATE(io, chr_enabled);
    STATE(io, nametable_page);
    STATE(io, one_screen);
    STATE(io, vertical_mirror);
    if (chr_is_ram) {
        state_bytes(io, "discrete.chr_ram", chr_storage, CHR_RAM_SIZE);
    }
#ifdef MESEN_COMPATIBILITY
    STATE(io, work_ram);
#endif
    if (board->state != NULL) {
        board->state(io);
    }
    if (state_loading(io)) {
        // (also a safe wrap if the state came from another ROM)
        if (board->prg_pair) {
            discrete_select_prg_pair(prg_bank, prg_bank_high);
        } else {
            discrete_select_prg(prg_bank);
        }
        if (board->chr_4k) {
            discrete_select_chr4(chr_bank, chr_bank_high);
        } else {
            discrete_select_chr(chr_bank);
        }
    }
    STATE_SKIP(board); // NOLINT(bugprone-sizeof-expression): the macro only names it; the wiring comes from the ROM
    STATE_SKIP(prg_rom_storage);
    STATE_SKIP(prg_size_bytes);
    STATE_SKIP(prg_bank_count);
    STATE_SKIP(prg_offset);
    STATE_SKIP(chr_storage);
    STATE_SKIP(chr_size_bytes);
    STATE_SKIP(chr_bank_count);
    STATE_SKIP(chr_offset);
    STATE_SKIP(chr_is_ram);
    STATE_SKIP(board_submapper);
    STATE_SKIP(bus_conflicts);
}

bool mapper_discrete_init(ines_data *ines_file_data, mapper_info_t *p_mapper_info, const discrete_board_t *b)
{
    assert(ines_file_data != NULL);
    assert(p_mapper_info != NULL);
    assert(b != NULL);
    board = b;
    vertical_mirror = (0 != ines_get_mirroring(ines_file_data));
    one_screen = board->one_screen;
    LOG_DEBUG(MAPPER, "%s: mirroring %s", board->name,
              board->one_screen ? "from the register"
              : vertical_mirror ? "vertical"
                                : "horizontal");

    // Bus conflicts. NES 2.0 submapper 1 = no conflicts, 2 = AND conflicts, 0 = unspecified.
    uint8_t submapper = 0;
    if ((ines_file_data->header.flags_7 & 0x0C) == 0x08) {
        submapper = ines_file_data->header.prg_ram_8kb_banks >> 4; // NES 2.0 byte 8 upper nibble
    }
#ifdef MESEN_COMPATIBILITY
    bus_conflicts = (2 == submapper); // Mesen only emulates conflicts for submapper 2
#else
    bus_conflicts = (1 != submapper); // the original boards have bus conflicts
#endif
    board_submapper = submapper;
    if (board->no_bus_conflicts) {
        bus_conflicts = false;
    }
    LOG_DEBUG(MAPPER, "submapper %d, bus conflicts %s", submapper, bus_conflicts ? "on" : "off");

    prg_size_bytes = 16 * 1024 * ines_file_data->header.prg_16kb_banks;
    LOG_DEBUG(MAPPER, "PRG size: %d (0x%X)", prg_size_bytes, prg_size_bytes);
    if (0 == prg_size_bytes || (prg_size_bytes & 0x3FFF) != 0 ||
        (board->prg_bank_size == 0x8000 && prg_size_bytes != 0x4000 && (prg_size_bytes & 0x7FFF) != 0)) {
        LOG_ERROR(MAPPER, "%s: unsupported PRG size", board->name);
        return false;
    }
    prg_rom_storage = malloc(prg_size_bytes);
    if (NULL == prg_rom_storage) {
        LOG_ERROR(MAPPER, "unable to allocate PRG storage");
        return false;
    }
    memcpy(prg_rom_storage, ines_file_data->prg_rom, prg_size_bytes);
    // a 16KB ROM on a 32KB board shows twice (as on NROM / CNROM)
    prg_bank_count = prg_size_bytes / board->prg_bank_size;
    if (0 == prg_bank_count) {
        prg_bank_count = 1;
    }

    chr_bank_count = ines_file_data->header.chr_8kb_banks;
    chr_size_bytes = chr_bank_count * 8192;
    if (0 == chr_bank_count) {
        if (board->chr_rom_banked) {
            LOG_ERROR(MAPPER, "%s without CHR-ROM is not supported", board->name);
            discrete_deinit();
            return false;
        }
        chr_is_ram = true;
        chr_size_bytes = CHR_RAM_SIZE;
        chr_bank_count = 1;
        chr_storage = (uint8_t *)calloc(1, CHR_RAM_SIZE); // powers on as $00 (matches Mesen's default)
    } else {
        chr_is_ram = false;
        chr_storage = (uint8_t *)malloc(chr_size_bytes);
        if (chr_storage != NULL) {
            memcpy(chr_storage, ines_file_data->chr_rom, chr_size_bytes);
        }
    }
    if (chr_storage == NULL) {
        LOG_ERROR(MAPPER, "Unable to allocate memory for CHR");
        discrete_deinit();
        return false;
    }
    // power-on banks 0 (matches Mesen with power-on randomization off)
    prg_bank = 0;
    chr_bank = 0;
    chr_bank_high = 0;
    prg_bank_high = 0;
    chr_enabled = true;
    nametable_page = 0;
    if (board->prg_pair) {
        discrete_select_prg_pair(0, 0);
    } else {
        discrete_select_prg(0);
    }
    if (board->chr_4k) {
        discrete_select_chr4(0, 0);
    } else {
        discrete_select_chr(0);
    }
    if (board->power_on != NULL) {
        board->power_on();
    }
#ifdef MESEN_COMPATIBILITY
    memset(work_ram, 0x00, sizeof(work_ram)); // Mesen's default power-on RAM state
#endif

    p_mapper_info->deinit = discrete_deinit;
    p_mapper_info->read_prg = discrete_read_prg;
    p_mapper_info->write_prg = discrete_write_prg;
    p_mapper_info->read_chr = discrete_read_chr;
    p_mapper_info->write_chr = discrete_write_chr;
    p_mapper_info->get_ciram_addr = discrete_get_ciram_addr;
    p_mapper_info->debug_read_chr = discrete_read_chr;
    p_mapper_info->chr_is_open = board->chr_gate ? discrete_chr_is_open : NULL;
    p_mapper_info->debug_read_prg = discrete_debug_read_prg;
    p_mapper_info->debug_chr_offset = discrete_debug_chr_offset;
    p_mapper_info->debug_chr_data = discrete_debug_chr_data;
    p_mapper_info->state = discrete_state;
    if (prg_bank_count > 1) {
        p_mapper_info->debug_prg_offset = discrete_debug_prg_offset;
        p_mapper_info->prg_bank_size = board->prg_bank_size;
    }
    return true;
}

static void discrete_deinit(void)
{
    LOG_DEBUG(MAPPER, "free'ing PRG and CHR.");
    free(prg_rom_storage);
    prg_rom_storage = NULL;
    free(chr_storage);
    chr_storage = NULL;
}

void discrete_select_prg(unsigned bank)
{
    prg_bank = bank % prg_bank_count;
    if (board->fixed_last_prg) {
        prg_offset[0] = prg_bank * 0x4000;
        prg_offset[1] = prg_size_bytes - 0x4000;
    } else if (prg_size_bytes == 0x4000) {
        prg_offset[0] = prg_offset[1] = 0;
    } else {
        prg_offset[0] = prg_bank * 0x8000;
        prg_offset[1] = prg_offset[0] + 0x4000;
    }
}

void discrete_select_chr(unsigned bank)
{
    chr_bank = bank % chr_bank_count;
    chr_offset[0] = chr_bank * 0x2000;
    chr_offset[1] = chr_offset[0] + 0x1000;
}

void discrete_select_chr4(unsigned low, unsigned high)
{
    chr_bank = low % (chr_bank_count * 2);
    chr_bank_high = high % (chr_bank_count * 2);
    chr_offset[0] = chr_bank * 0x1000;
    chr_offset[1] = chr_bank_high * 0x1000;
}

void discrete_select_prg_pair(unsigned low, unsigned high)
{
    prg_bank = low % prg_bank_count;
    prg_bank_high = high % prg_bank_count;
    prg_offset[0] = prg_bank * 0x4000;
    prg_offset[1] = prg_bank_high * 0x4000;
}

void discrete_enable_chr(bool on)
{
    chr_enabled = on;
}

void discrete_select_page(unsigned page)
{
    nametable_page = page & 1;
    one_screen = true;
}

void discrete_select_mirroring(bool vertical)
{
    vertical_mirror = vertical;
    one_screen = false;
}

void discrete_set_submapper(unsigned submapper)
{
    board_submapper = (uint8_t)submapper;
}

unsigned discrete_submapper(void)
{
    return board_submapper;
}

static uint8_t discrete_read_prg(uint16_t address)
{
    if (address < 0x6000) {
        assert(0); // cpu_read handles $4019-$5FFF
        return 0;
    } else if (address < 0x8000) {
#ifdef MESEN_COMPATIBILITY
        if (board->mesen_work_ram) {
            return work_ram[address & (WORK_RAM_SIZE - 1)];
        }
#endif
        // no PRG-RAM on these boards: nothing drives the bus
        return cpu_get_open_bus();
    }
    return prg_rom_storage[prg_offset[(address >> 14) & 1] + (address & 0x3FFF)];
}

static void discrete_write_latch(uint16_t address, uint8_t value)
{
    if (board->write_reg_at != NULL) {
        board->write_reg_at(address, value);
    } else {
        board->write_reg(value);
    }
}

static void discrete_write_prg(uint16_t address, uint8_t value)
{
    if (board->reg_low) {
        if ((address & 0xE100) == 0x4100) {
            discrete_write_latch(address, value);
        }
        return;
    }
    if (address < 0x6000) {
        return;
    } else if (address < 0x8000) {
        if (board->reg_6000) {
            discrete_write_latch(address, value);
            return;
        }
#ifdef MESEN_COMPATIBILITY
        if (board->mesen_work_ram) {
            work_ram[address & (WORK_RAM_SIZE - 1)] = value;
        }
#endif
        // (no PRG-RAM on a real board: the write goes nowhere)
        return;
    }
    if (board->reg_6000) {
        return; // the latch is at $6000-$7FFF, not here
    }
    if (bus_conflicts) {
        value &= discrete_read_prg(address);
    }
    discrete_write_latch(address, value);
}

static bool discrete_chr_is_open(void)
{
    return !chr_enabled;
}

static uint8_t discrete_read_chr(uint16_t address)
{
    assert(address < 0x2000);
    if (!chr_enabled) {
        return (uint8_t)address;
    }
    return chr_storage[(address & 0x0FFF) + chr_offset[(address >> 12) & 1]];
}

static void discrete_write_chr(uint16_t address, uint8_t value)
{
    assert(address < 0x2000);
    if (chr_is_ram) {
        chr_storage[address & 0x1FFF] = value;
    }
}

// input is the PPU address trying to be accessed
// return value is the ciram address to use
// this is how mirroring is implemented
static uint16_t discrete_get_ciram_addr(uint16_t addr)
{
    if (one_screen) {
        return (addr & 0x03FF) | (uint16_t)(nametable_page << 10);
    } else if (vertical_mirror) {
        return (addr & 0x07FF); // ciram A10 set to PPU A10
    }
    return (addr & 0x03FF) | ((addr & BIT_11) >> 1); // ciram A10 set to PPU A11
}

// Debugger / trace peek: where nothing drives the bus ($6000-$7FFF without work RAM) Mesen shows the address high byte
static uint8_t discrete_debug_read_prg(uint16_t address)
{
#ifdef MESEN_COMPATIBILITY
    bool has_ram = board->mesen_work_ram;
#else
    bool has_ram = false;
#endif
    if (address < 0x8000 && !has_ram) {
        return (uint8_t)(address >> 8);
    }
    return discrete_read_prg(address);
}

static int32_t discrete_debug_prg_offset(uint16_t address)
{
    if (address < 0x8000) {
        return -1;
    }
    return (int32_t)(prg_offset[(address >> 14) & 1] + (address & 0x3FFF));
}

static int32_t discrete_debug_chr_offset(uint16_t address)
{
    return (int32_t)((address & 0x0FFF) + chr_offset[(address >> 12) & 1]);
}

static const uint8_t *discrete_debug_chr_data(size_t *size)
{
    *size = chr_size_bytes;
    return chr_storage;
}
