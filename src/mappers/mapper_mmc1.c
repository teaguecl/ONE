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

/********************************* REVISIONS ***********************************
 * 12-05-2001   CLT   File created
 *******************************************************************************/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "log.h"
#include "mapper_mmc1.h"
#include "mapper.h"
#include "bits.h"
#include "ines.h"
#include "cpu.h"
#include "savestate.h"

#define SRAM_SIZE (8192)

static uint8_t *prg_rom_storage = NULL; /* holds the physical data for program ROM */
static uint32_t prg_size_bytes = 0;
static bool chr_is_ram = false;
static uint8_t *chr_rxm_storage = NULL; /* holds the physical data for character ROM/RAM */
static uint32_t chr_size_bytes = 0;     /* CHR-ROM size, or 8KB of CHR-RAM */
static uint8_t *save_ram = NULL;        /* WRAM or Save RAM at $6000 (NULL: none) */
static uint32_t save_ram_size = 0;      /* its size: SRAM_SIZE, or what an NES 2.0 header says (a power of two) */

#define INITIAL_SHIFT_REG (0x10)

static uint8_t shift_reg = INITIAL_SHIFT_REG;
static uint8_t control_reg = 0x0C;
static uint8_t chr_bank0 = 0;
static uint8_t chr_bank1 = 0;
static uint8_t prg_bank = 0;

static uint64_t last_write_cycle = 0;

// Byte offset into prg_rom_storage of the 16KB bank mapped at $8000 [0] and $C000 [1].
// Recomputed only when control_reg or prg_bank changes, so a PRG read is a single lookup.
static uint32_t prg_slot_offset[2];
static uint32_t chr_slot_offset[2];

// PRG banking, branch-free.  hi = CPU A14 (0: $8000 slot, 1: $C000 slot), p = PRG bank register.
//   modes 0/1 (32KB):              bank = (p & ~1) | hi
//   mode 2 (fix first at $8000):   hi=0 -> 0,  hi=1 -> p
//   mode 3 (fix last at $C000):    hi=0 -> p,  hi=1 -> last
// In the 16KB modes the switchable slot is the one where hi != mode bit 0 (sw = 1), and the fixed
// slot holds bank 0 when hi=0 (mode 2) or the last bank when hi=1 (mode 3), i.e. last & -hi.
// Each result is selected with all-ones/all-zero masks instead of branches.  The final '% banks'
// handles ROM sizes that are not a power of two (e.g. 80KB cpu_interrupts.nes).
static void mapper_mmc1_update_prg_banks(void)
{
    uint32_t banks = prg_size_bytes / 0x4000; // number of 16KB banks
    uint32_t mode = (control_reg >> 2) & 3;
    uint32_t p = prg_bank & 0x0F;                   // bit 4 is the PRG-RAM disable
    uint32_t mask32 = 0u - (((mode >> 1) & 1) ^ 1); // all ones in 32KB modes (0, 1)

    for (uint32_t hi = 0; hi < 2; hi++) {
        uint32_t sw = hi ^ (mode & 1); // 16KB modes: 1 = this slot is switchable
        uint32_t bank16 = (p & (0u - sw)) | ((banks - 1) & (0u - hi) & (0u - (sw ^ 1)));
        uint32_t bank32 = (p & 0x0E) | hi;
        uint32_t bank = (bank32 & mask32) | (bank16 & ~mask32);
        prg_slot_offset[hi] = (bank % banks) * 0x4000;
    }
}

// Byte offset into chr_rxm_storage of the 4KB bank mapped at PPU $0000 [0] and $1000 [1].
// Recomputed only when control_reg, chr_bank0 or chr_bank1 changes, so a CHR access is a single lookup.
//   control bit 4 = 1 (two 4KB banks):  hi=0 -> chr_bank0, hi=1 -> chr_bank1
//   control bit 4 = 0 (one 8KB bank):   bank = (chr_bank0 & ~1) | hi
// hi = PPU A12.  Both results are selected with all-ones/all-zero masks instead of branches.
// '% banks' wraps bank numbers larger than the CHR size (8KB of CHR-RAM = 2 banks; on SUROM the
// CHR register's bit 4 drives PRG A18 instead and is dropped here the same way).
static void mapper_mmc1_update_chr_banks(void)
{
    uint32_t banks = chr_size_bytes / 0x1000;             // number of 4KB banks
    uint32_t mask8 = 0u - (((control_reg >> 4) & 1) ^ 1); // all ones in 8KB mode

    for (uint32_t hi = 0; hi < 2; hi++) {
        uint32_t mhi = 0u - hi; // all ones for the $1000 slot
        uint32_t bank4 = (chr_bank0 & ~mhi) | (chr_bank1 & mhi);
        uint32_t bank8 = (chr_bank0 & 0x1E) | hi;
        uint32_t bank = ((bank8 & mask8) | (bank4 & ~mask8)) & 0x1F;
        chr_slot_offset[hi] = (bank % banks) * 0x1000;
    }
}


/*******************************************************************************
 * FUNCTION NAME : mapper_nrom_deinit()
 * ARGUMENTS     : void
 * RETURN        : void
 * DESCRIPTION   : This function will be called at exit because it is registered
 *                 using the atexit() library call.  It will free the memory
 *                 which has been malloc'd by mapper_mmc1_init()
 *******************************************************************************/
static void mapper_mmc1_deinit(void)
{
    LOG_DEBUG(MAPPER, "free'ing PRG.");
    free(prg_rom_storage);
    prg_rom_storage = NULL;

    free(save_ram);
    save_ram = NULL;

    LOG_DEBUG(MAPPER, "free'ing CHR.");
    free(chr_rxm_storage);
    chr_rxm_storage = NULL;
}

static uint8_t mapper_mmc1_read_prg(uint16_t address)
{
    uint8_t val = 0xa5;
    if (address < 0x6000) {
        // read from ???
        val = 0;
    } else if (address < 0x8000) {
        if (prg_bank & BIT_4) {
            val = cpu_get_open_bus();
        } else {
            // read from SRAM
            if (NULL != save_ram) {
                return (save_ram[address & (save_ram_size - 1)]); // (less than 8KB: mirrored)
            }
            val = cpu_get_open_bus(); // no PRG-RAM on the board (NES 2.0)
        }
    } else {
        return prg_rom_storage[prg_slot_offset[(address >> 14) & 1] | (address & 0x3FFF)];
    }
    return val;
}


static void mapper_mmc1_write_prg(uint16_t address, uint8_t value)
{
    if (address < 0x6000) {
        // read from ???
        return;
    } else if (address < 0x8000) {
        // write to SRAM
        if (!(prg_bank & BIT_4)) {
            if (NULL != save_ram) {
                save_ram[address & (save_ram_size - 1)] = value;
            } else {
                LOG_TRACE(MAPPER, "write to unallocated SRAM");
            }
        }
    } else {
        uint64_t current_cpu_clock = cpu_get_cycle();
        if (value & BIT_7) {
            // reset
            shift_reg = INITIAL_SHIFT_REG;
            control_reg |= 0x0C;
            mapper_mmc1_update_prg_banks();
            mapper_mmc1_update_chr_banks();
        } else if ((current_cpu_clock - last_write_cycle) >= 2) {
            if (shift_reg & BIT_0) {
                // shift reg full
                uint8_t val = (shift_reg >> 1) | ((value & BIT_0) << 4);
                shift_reg = INITIAL_SHIFT_REG;
                if (address < 0xA000) {
                    control_reg = val;
                } else if (address < 0xC000) {
                    chr_bank0 = val;
                } else if (address < 0xE000) {
                    chr_bank1 = val;
                } else {
                    prg_bank = val;
                }
                mapper_mmc1_update_prg_banks();
                mapper_mmc1_update_chr_banks();
            } else {
                // write to shift reg
                shift_reg = (shift_reg >> 1) | ((value & BIT_0) << 4);
            }
        }
        last_write_cycle = current_cpu_clock;
    }
}

static uint8_t mapper_mmc1_read_chr(uint16_t address)
{
    assert(address < 0x2000);
    return chr_rxm_storage[chr_slot_offset[(address >> 12) & 1] | (address & 0x0FFF)];
}

static void mapper_mmc1_write_chr(uint16_t address, uint8_t value)
{
    assert(address < 0x2000);
    if (chr_is_ram) {
        chr_rxm_storage[chr_slot_offset[(address >> 12) & 1] | (address & 0x0FFF)] = value;
    } else {
        LOG_TRACE(MAPPER, "Attempt to write to chr_rom  [%04X] = %02X", address, value);
    }
}

// input is the PPU address trying to be accessed
// return value is the ciram address to use
// this is how mirroring is implemented
//
// CIRAM A0-A9 always come from PPU A0-A9; only CIRAM A10 depends on the mirroring mode
// (control register bits 0-1):
//   0: one-screen, lower bank  -> A10 = 0
//   1: one-screen, upper bank  -> A10 = 1
//   2: vertical                -> A10 = PPU A10
//   3: horizontal              -> A10 = PPU A11
// Branch-free: for modes 2/3 (mode bit 1 set) take PPU bit (10 + mode bit 0), otherwise
// take mode bit 0 itself.  'use_ppu' is all ones for modes 2/3 and 0 for modes 0/1.
static uint16_t mapper_mmc1_get_ciram_addr(uint16_t addr)
{
    unsigned mode = control_reg & 3;
    unsigned use_ppu = 0u - (mode >> 1); // 0x00000000 or 0xFFFFFFFF
    unsigned a10 = (((addr >> (10 + (mode & 1))) & 1) & use_ppu) | ((mode & 1) & ~use_ppu);
    return (addr & 0x3FF) | (a10 << 10);
}


static uint8_t mapper_mmc1_debug_read_chr(uint16_t address)
{
    assert(address < 0x2000);
    return mapper_mmc1_read_chr(address);
}

static int32_t mapper_mmc1_debug_chr_offset(uint16_t address)
{
    return (int32_t)(chr_slot_offset[(address >> 12) & 1] | (address & 0x0FFF));
}

static const uint8_t *mapper_mmc1_debug_chr_data(size_t *size)
{
    *size = chr_size_bytes;
    return chr_rxm_storage;
}

static int32_t mapper_mmc1_debug_prg_offset(uint16_t address)
{
    return (int32_t)(prg_slot_offset[(address >> 14) & 1] | (address & 0x3FFF));
}

static uint8_t *mapper_mmc1_debug_prg_ram(size_t *size)
{
    *size = (NULL != save_ram) ? save_ram_size : 0;
    return save_ram;
}

static void mapper_mmc1_get_sav_data(sav_data_t *sav)
{
    sav->data = save_ram; // still owned by this mapper
    sav->size = (NULL != save_ram) ? save_ram_size : 0;
}

// Save states: the registers, the bank offsets they select, and the RAM
static void mapper_mmc1_state(state_io *io)
{
    STATE(io, shift_reg);
    STATE(io, control_reg);
    STATE(io, chr_bank0);
    STATE(io, chr_bank1);
    STATE(io, prg_bank);
    STATE(io, last_write_cycle);
    STATE(io, prg_slot_offset);
    STATE(io, chr_slot_offset);
    if (save_ram != NULL) {
        state_bytes(io, "mmc1.save_ram", save_ram, save_ram_size);
    }
    if (chr_is_ram) {
        state_bytes(io, "mmc1.chr_ram", chr_rxm_storage, chr_size_bytes);
    }
    STATE_SKIP(save_ram); // (the pointers and sizes come from the ROM)
    STATE_SKIP(save_ram_size);
    STATE_SKIP(chr_rxm_storage);
    STATE_SKIP(chr_size_bytes);
    STATE_SKIP(chr_is_ram);
    STATE_SKIP(prg_rom_storage);
    STATE_SKIP(prg_size_bytes);
}

// The NES 2.0 header fields MMC1 uses (mapper_nes2_info()): the submapper, one RAM chip at $6000 (battery-backed or
// not) and the CHR-RAM size. Sets save_ram_size and *chr_ram_size; false (logged) if ONE cannot run the ROM.
static bool mapper_mmc1_nes2_header(const ines_header *h, uint32_t *chr_ram_size)
{
    nes2_info_t nes2;
    if (!mapper_nes2_info(h, &nes2)) {
        return false;
    }
    switch (nes2.submapper) {
    case 0: break;
    case 5: LOG_WARN(MAPPER, "NES 2.0 submapper 5 (SEROM/SHROM, fixed PRG) is not supported: run as SxROM"); break;
    case 6: LOG_WARN(MAPPER, "NES 2.0 submapper 6 (2ME) is not supported: run as SxROM"); break;
    default: LOG_WARN(MAPPER, "NES 2.0 submapper %u is not supported: run as SxROM", nes2.submapper); break;
    }
    if ((0 != nes2.prg_ram_size) && (0 != nes2.prg_nvram_size)) { // SOROM: 8KB + 8KB battery, banked
        LOG_WARN(MAPPER, "NES 2.0 header asks for both PRG-RAM and battery RAM: using the battery RAM");
    }
    save_ram_size = (0 != nes2.prg_nvram_size) ? nes2.prg_nvram_size : nes2.prg_ram_size;
    if (save_ram_size > SRAM_SIZE) { // SXROM's 32KB is banked by the CHR registers: not emulated
        LOG_WARN(MAPPER, "NES 2.0 PRG-RAM of %u bytes: only 8KB (no PRG-RAM banking)", save_ram_size);
        save_ram_size = SRAM_SIZE;
    }
    if (0 != nes2.chr_ram_size) {
        *chr_ram_size = (nes2.chr_ram_size < 8 * 1024) ? 8 * 1024 : nes2.chr_ram_size; // (the banking needs 4KB banks)
    }
    return true;
}

bool mapper_mmc1_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data)
{

    assert(ines_file_data != NULL);
    assert(p_mapper_info != NULL);
    shift_reg = INITIAL_SHIFT_REG;
    control_reg = 0x0C;
    chr_bank0 = 0;
    chr_bank1 = 0;
    prg_bank = 0;
    last_write_cycle = 0;

    // header first, before anything is allocated
    uint32_t chr_ram_size = 8 * 1024; // with no CHR-ROM (iNES 1.0: always 8KB)
    save_ram_size = SRAM_SIZE;        // iNES 1.0: always 8KB
    if (mapper_is_nes2(&ines_file_data->header)) {
        if (!mapper_mmc1_nes2_header(&ines_file_data->header, &chr_ram_size)) {
            return false;
        }
    } else if (ines_file_data->header.prg_ram_8kb_banks > 1) {
        LOG_WARN(MAPPER, "ROM asked for %d PRG RAM banks, but only allocating 1",
                 ines_file_data->header.prg_ram_8kb_banks);
        return false;
    }

    prg_size_bytes = 16 * 1024 * ines_file_data->header.prg_16kb_banks;
    prg_rom_storage = malloc(prg_size_bytes);
    if (NULL == prg_rom_storage) {
        LOG_ERROR(MAPPER, "unable to allocate PRG storage");
        return false;
    }
    memcpy(prg_rom_storage, ines_file_data->prg_rom, prg_size_bytes);
    mapper_mmc1_update_prg_banks(); // power-on mapping (control $0C: mode 3)

    chr_size_bytes = ines_file_data->header.chr_8kb_banks * 8192;
    if (0 == chr_size_bytes) {
        // then there is CHR_RAM (8KB, or the NES 2.0 size) and no CHR_ROM
        chr_is_ram = true;
        chr_size_bytes = chr_ram_size;
        chr_rxm_storage = (uint8_t *)calloc(1, chr_size_bytes);
        if (chr_rxm_storage == NULL) {
            LOG_ERROR(MAPPER, "Unable to allocate %u bytes of memory.", chr_size_bytes);
            free(prg_rom_storage);
            prg_rom_storage = NULL;
            return false;
        }
    } else {
        chr_is_ram = false;
        chr_rxm_storage = (uint8_t *)malloc(chr_size_bytes);
        if (chr_rxm_storage == NULL) {
            LOG_ERROR(MAPPER, "Unable to allocate memory for chr_rom");
            free(prg_rom_storage);
            prg_rom_storage = NULL;
            return false;
        }
        memcpy(chr_rxm_storage, ines_file_data->chr_rom, chr_size_bytes);
    }

    mapper_mmc1_update_chr_banks(); // power-on mapping (control $0C: 8KB mode, bank 0)

    save_ram = NULL;
    if (0 != save_ram_size) {
        save_ram = calloc(1, save_ram_size);
        if (NULL == save_ram) {
            LOG_ERROR(MAPPER, "Unable to allocate SRAM");
            free(prg_rom_storage);
            prg_rom_storage = NULL;
            free(chr_rxm_storage);
            chr_rxm_storage = NULL;
            return false;
        }
        if ((ines_file_data->header.flags_6 & BIT_1) && (sav_data->size > 0)) { // battery: load the .sav file
            if (sav_data->size != save_ram_size) {
                LOG_WARN(MAPPER, ".sav file is %zu bytes, expected %u; using what fits", sav_data->size, save_ram_size);
            }
            memcpy(save_ram, sav_data->data, (sav_data->size < save_ram_size) ? sav_data->size : save_ram_size);
        }
    }

    p_mapper_info->deinit = mapper_mmc1_deinit;
    p_mapper_info->read_prg = mapper_mmc1_read_prg;
    p_mapper_info->write_prg = mapper_mmc1_write_prg;
    p_mapper_info->read_chr = mapper_mmc1_read_chr;
    p_mapper_info->write_chr = mapper_mmc1_write_chr;
    p_mapper_info->get_ciram_addr = mapper_mmc1_get_ciram_addr;
    p_mapper_info->debug_read_chr = mapper_mmc1_debug_read_chr;
    p_mapper_info->get_sav_data = mapper_mmc1_get_sav_data;
    p_mapper_info->debug_prg_ram = mapper_mmc1_debug_prg_ram;
    p_mapper_info->debug_prg_offset = mapper_mmc1_debug_prg_offset;
    p_mapper_info->prg_bank_size = 0x4000; // (16KB banks; SxROM's 32KB mode switches two of them)
    p_mapper_info->debug_chr_offset = mapper_mmc1_debug_chr_offset;
    p_mapper_info->debug_chr_data = mapper_mmc1_debug_chr_data;
    p_mapper_info->state = mapper_mmc1_state;

    LOG_DEBUG(MAPPER, "Successfully loaded mmc1");
    return true;
    //  return false;
}
