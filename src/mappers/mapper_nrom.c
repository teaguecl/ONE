/*
  SPDX-License-Identifier: GPL-2.0-or-later
  ONE NES emulator
  Copyright (C) 1999-2017 Chris Teague

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
#include <string.h>
#include <assert.h>
#include "ines.h"
#include "log.h"
#include "mapper_nrom.h"
#include "mapper.h"
#include "bits.h"
#include "savestate.h"

#define SRAM_SIZE (0x2000) // 8KB SRAM located in CPU address space $6000-$7FFF

static uint8_t *save_ram = NULL;
static uint8_t *mapper_nrom_debug_prg_ram(size_t *size);
static uint8_t *prg_rom_storage = NULL; /* holds the physical data */
static uint32_t prg_size_bytes = 0;
static uint8_t *lower_prg_rom = NULL; /* maps to addresses 0x8000 - 0xBFFF */
static uint8_t *upper_prg_rom = NULL; /* maps to addresses 0xC000 - 0xFFFF*/

static uint8_t *chr_rxm_storage = NULL; /* holds the physical data for chr_rom/chr_ram */
static uint32_t chr_size_bytes = 0;
static bool chr_is_ram = false;
static int32_t mapper_nrom_debug_chr_offset(uint16_t address);
static const uint8_t *mapper_nrom_debug_chr_data(size_t *size);
static bool horizontal_mirror = false;


/*******************************************************************************
 * FUNCTION NAME : mapper_nrom_init_ines()
 * ARGUMENTS     : ines_data *ines_file_data - pointer to structure which
 *			    contains the INES chunks that were read from the file.
 * RETURN        : NO_ERROR or ERROR
 * DESCRIPTION   : This function will copy all of the data out of the ines_data
 *                 structure which the NROM mapper requires.  It will register
 *                 read and write functions with the memory module for the memory
 *                 locations from 0x8000 - 0xFFFF
 *******************************************************************************/
// Save states: NROM has no registers, only its RAM
static void mapper_nrom_state(state_io *io)
{
    if (save_ram != NULL) {
        state_bytes(io, "nrom.save_ram", save_ram, SRAM_SIZE);
    }
    if (chr_is_ram) {
        state_bytes(io, "nrom.chr_ram", chr_rxm_storage, 8 * 1024); // (chr_size_bytes is 0 with CHR-RAM)
    }
    STATE_SKIP(save_ram); // (the pointers and sizes come from the ROM)
    STATE_SKIP(chr_rxm_storage);
    STATE_SKIP(chr_size_bytes);
    STATE_SKIP(chr_is_ram);
    STATE_SKIP(prg_rom_storage);
    STATE_SKIP(prg_size_bytes);
    STATE_SKIP(lower_prg_rom);
    STATE_SKIP(upper_prg_rom);
    STATE_SKIP(horizontal_mirror);
}

bool mapper_nrom_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    assert(ines_file_data != NULL);
    assert(p_mapper_info != NULL);
    if (0 == ines_get_mirroring(ines_file_data)) {
        horizontal_mirror = false;
        LOG_DEBUG(MAPPER, "Mirroring set to Vertical");
    } else {
        horizontal_mirror = true;
        LOG_DEBUG(MAPPER, "Mirroring set to Horizontal");
    }

    prg_size_bytes = 16 * 1024 * ines_file_data->header.prg_16kb_banks;
    prg_rom_storage = malloc(prg_size_bytes);
    if (NULL == prg_rom_storage) {
        LOG_ERROR(MAPPER, "unable to allocate PRG storage");
        return false;
    }
    memcpy(prg_rom_storage, ines_file_data->prg_rom, prg_size_bytes);
    lower_prg_rom = prg_rom_storage;
    if (prg_size_bytes == 0x4000) {
        /* then this cart needs to map the upper bank to the lower bank */
        LOG_DEBUG(MAPPER, "Mirroring 0xC000 - 0xFFFF to 0x8000 - 0xBFFF");
        upper_prg_rom = lower_prg_rom;
    } else {
        LOG_DEBUG(MAPPER, "No PRG mirroring needed.");
        upper_prg_rom = &prg_rom_storage[0x4000];
    }

    chr_size_bytes = ines_file_data->header.chr_8kb_banks * 8192;
    if (0 == chr_size_bytes) {
        // then there is 8KB of CHR_RAM and no CHR_ROM
        chr_is_ram = true;
        chr_rxm_storage = (uint8_t *)malloc(8 * 1024);
        if (chr_rxm_storage == NULL) {
            LOG_ERROR(MAPPER, "Unable to allocate %u bytes of memory.", 8 * 1024);
            free(prg_rom_storage);
            prg_rom_storage = NULL;
            return false;
        }
        memset(chr_rxm_storage, 0x00, 8 * 1024); // CHR-RAM powers on as $00 (matches Mesen's default power-on state)
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

    save_ram = NULL;
    if (ines_file_data->header.prg_ram_8kb_banks > 1) {
        LOG_WARN(MAPPER, "ROM asked for %d PRG RAM banks, but only allocating 1",
                 ines_file_data->header.prg_ram_8kb_banks);
        return false;
    }
    save_ram = malloc(SRAM_SIZE);
    if (NULL == save_ram) {
        LOG_ERROR(MAPPER, "Unable to allocate SRAM");
        return false;
    }
    memset(save_ram, 0x00,
           SRAM_SIZE); // Mesen inits to 0x00 (Nintendulator used 0xFF).  Match Mesen so traces are compatible

#if 0
 /* now register the read and write functions with the memory module */
  LOG_DEBUG(MAPPER, "Registering read and write functions.");
  mem_register_read_function(mapper_nrom_read_byte_lower_prg, lower_prg_rom, 0x8000, 0x4000);
  mem_register_write_function(mapper_nrom_write_byte_lower_prg, 0x8000, 0x4000);
  mem_register_read_function(mapper_nrom_read_byte_upper_prg, upper_prg_rom, 0xC000, 0x4000);
  mem_register_write_function(mapper_nrom_write_byte_upper_prg, 0xC000, 0x4000);
#else
    p_mapper_info->deinit = mapper_nrom_deinit;
    p_mapper_info->read_prg = mapper_nrom_read_prg;
    p_mapper_info->write_prg = mapper_nrom_write_prg;
    p_mapper_info->read_chr = mapper_nrom_read_chr;
    p_mapper_info->write_chr = mapper_nrom_write_chr;
    p_mapper_info->get_ciram_addr = mapper_nrom_get_ciram_addr;
    p_mapper_info->debug_read_chr = mapper_nrom_debug_read_chr;
    p_mapper_info->debug_chr_offset = mapper_nrom_debug_chr_offset;
    p_mapper_info->debug_chr_data = mapper_nrom_debug_chr_data;
    p_mapper_info->debug_prg_ram = mapper_nrom_debug_prg_ram;
    p_mapper_info->state = mapper_nrom_state;
#endif

    return true;
}

/*******************************************************************************
 * FUNCTION NAME : mapper_nrom_read_byte_lower_prg()
 * ARGUMENTS     : uint16_t - address to read from
 * RETURN        : uint8_t - data read from the mapper memory
 * DESCRIPTION   : This function will retrieve data to be read from the mapper.
 *******************************************************************************/
uint8_t mapper_nrom_read_byte_lower_prg(uint16_t address)
{
    return (lower_prg_rom[address - 0x8000]);
}


/*******************************************************************************
 * FUNCTION NAME : mapper_nrom_read_byte_upper_prg()
 * ARGUMENTS     : uint16_t - address to read from
 * RETURN        : uint8_t - data read from the mapper memory
 * DESCRIPTION   : This function will retrieve data to be read from the mapper.
 *******************************************************************************/
uint8_t mapper_nrom_read_byte_upper_prg(uint16_t address)
{
    return (upper_prg_rom[address - 0xC000]);
}

/*******************************************************************************
 * FUNCTION NAME : mapper_nrom_write_byte_lower_prg()
 * ARGUMENTS     : uint16_t - address to write to
 *                 uint8_t - data written to the mapper memory
 * RETURN		  : void
 * DESCRIPTION   : This function will write to the mapper's memory
 *******************************************************************************/
void mapper_nrom_write_byte_lower_prg(uint16_t address, uint8_t value)
{
    lower_prg_rom[address - 0x8000] = value;
}


/*******************************************************************************
 * FUNCTION NAME : mapper_nrom_write_byte_upper_prg()
 * ARGUMENTS     : uint16_t - address to write to
 *                 uint8_t - data written to the mapper memory
 * RETURN		  : void
 * DESCRIPTION   : This function will write to the mapper's memory
 *******************************************************************************/
void mapper_nrom_write_byte_upper_prg(uint16_t address, uint8_t value)
{
    upper_prg_rom[address - 0xC000] = value;
}

/*******************************************************************************
 * FUNCTION NAME : mapper_nrom_write_byte_chr()
 * ARGUMENTS     : uint16_t - address to write to
 *                 uint8_t - data written to the mapper memory
 * RETURN		  : void
 * DESCRIPTION   : This function will write to the mapper's memory
 *******************************************************************************/
void mapper_nrom_write_byte_chr(uint16_t address, uint8_t value) {}

/*******************************************************************************
 * FUNCTION NAME : mapper_nrom_deinit()
 * ARGUMENTS     : void
 * RETURN        : void
 * DESCRIPTION   : This function will be called at exit because it is registered
 *                 using the atexit() library call.  It will free the memory
 *                 which has been malloc'd by nrom_mapper_init()
 *******************************************************************************/
void mapper_nrom_deinit(void)
{
    LOG_DEBUG(MAPPER, "mapper_nrom_deinit: free'ing PRG.");
    if (NULL != prg_rom_storage) {
        free(prg_rom_storage);
        prg_rom_storage = NULL;
    }
    lower_prg_rom = NULL;
    upper_prg_rom = NULL;

    free(save_ram);
    save_ram = NULL;

    LOG_DEBUG(MAPPER, "mapper_nrom_deinit: free'ing CHR.");
    free(chr_rxm_storage);
    chr_rxm_storage = NULL;
}

static uint8_t *mapper_nrom_debug_prg_ram(size_t *size)
{
    *size = (NULL != save_ram) ? SRAM_SIZE : 0;
    return save_ram;
}

uint8_t mapper_nrom_read_prg(uint16_t address)
{
    if (address < 0x6000) {
        // read from ???
        return 0;
    } else if (address < 0x8000) {
        // read from SRAM
        if (NULL != save_ram) {
            return (save_ram[address - 0x6000]);
        } else {
            LOG_TRACE(MAPPER, "read from unallocated SRAM: %04X", address);
            return 0;
        }
    } else if (address < 0xC000) {
        return (lower_prg_rom[address - 0x8000]);
    } else {
        return (upper_prg_rom[address - 0xC000]);
    }
}

void mapper_nrom_write_prg(uint16_t address, uint8_t value)
{
    if (address < 0x6000) {
        // read from ???
        return;
    } else if (address < 0x8000) {
        // read from SRAM
        if (NULL != save_ram) {
            LOG_TRACE(MAPPER, "writing %X to nrom save_ram: %04X -> %04X", value, address, address - 0x6000);
            save_ram[address - 0x6000] = value;
            return;
        } else {
            LOG_TRACE(MAPPER, "write to unallocated SRAM");
            return;
        }
    } else if (address < 0xC000) {
        LOG_TRACE(MAPPER, "write to PRG ROM ignored: %04X", address);
        //lower_prg_rom[address - 0x8000] = value;
    } else {
        LOG_TRACE(MAPPER, "write to PRG ROM ignored: %04X", address);
        //upper_prg_rom[address - 0xC000] = value;
    }
}

uint8_t mapper_nrom_read_chr(uint16_t address)
{
    uint8_t value = 0xA5;

    assert(address < 0x2000);
    if (NULL != chr_rxm_storage) {
        value = chr_rxm_storage[address];
    }
    return value;
}

void mapper_nrom_write_chr(uint16_t address, uint8_t value)
{
    assert(address < 0x2000);
    if (NULL != chr_rxm_storage) {
        if (chr_is_ram) {
            chr_rxm_storage[address] = value;
        } else {
            LOG_TRACE(MAPPER, "Attempt to write to chr_rom  [%04X] = %02X", address, value);
        }
    }
}

// input is the PPU address trying to be accessed
// return value is the ciram address to use
// this is how mirroring is implemented
uint16_t mapper_nrom_get_ciram_addr(uint16_t addr)
{
    uint16_t ciram_addr;

    if (horizontal_mirror) {
        // ciram A10 set to PPU A10
        ciram_addr = (addr & 0x07FF); // clear out bits 9-15
    } else {
        ciram_addr = (addr & 0x03FF);         // clear out bits 10-15
        ciram_addr |= ((addr & BIT_11) >> 1); // ciram A10 set to PPU A11
    }
    return ciram_addr;
}


static int32_t mapper_nrom_debug_chr_offset(uint16_t address)
{
    return address & 0x1FFF; // (not banked)
}

static const uint8_t *mapper_nrom_debug_chr_data(size_t *size)
{
    *size = chr_is_ram ? 8 * 1024 : chr_size_bytes;
    return chr_rxm_storage;
}

uint8_t mapper_nrom_debug_read_chr(uint16_t address)
{
    assert(address < 0x2000);
    if (NULL != chr_rxm_storage) {
        return chr_rxm_storage[address];
    }
    return 0xA5;
}
