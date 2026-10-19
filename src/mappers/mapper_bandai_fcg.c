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

/*
 * Bandai FCG-1/2 and LZ93D50 (mapper 16): 16KB PRG bank at $8000 (last bank fixed at $C000), 8 x 1KB CHR banks (or 8KB CHR-RAM),
 * a CPU-cycle IRQ counter and a 24C02 serial EEPROM for the saves. The registers are written at $6000-$FFFF (address & $F):
 *     0-7  CHR bank (1KB) at $0000 ... $1C00      8  PRG bank (low 4 bits)      9  mirroring: 0 vertical, 1 horizontal, 2 / 3 one screen
 *     A    bit 0 = IRQ enabled; the counter is loaded from the latch; acknowledges the IRQ
 *     B C  the latch, low / high byte
 *     D    EEPROM: bit 5 = SCL, bit 6 = SDA, bit 7 = read enable
 *   A read of $6000-$7FFF returns the open bus with bit 4 = the EEPROM's data out.
 * Not done: the Datach barcode reader (157), the 24C01 (159), the FCG-1/2 counter (written directly, not through the latch).
 */

#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "bits.h"
#include "cpu.h"
#include "ines.h"
#include "log.h"
#include "mapper.h"
#include "mapper_bandai_fcg.h"
#include "savestate.h"

#define EEPROM_SIZE 256u

static uint8_t *prg_rom_storage = NULL;
static uint32_t prg_rom_size_bytes = 0;
static uint8_t *chr_storage = NULL;
static uint32_t chr_size_bytes = 0;
static bool chr_is_ram = false;
static uint32_t prg_slot_offset[2]; // 16KB slots
static uint32_t chr_slot_offset[8]; // 1KB slots
static uint8_t chr_reg[8];
static uint8_t prg_reg;
static bool prg_selected; // $8008 written (Mesen: $8000-$BFFF is open bus until then; only the trace shows it)
static uint8_t mirroring;
static uint16_t ciram_page[4];
static uint16_t irq_counter;
static uint16_t irq_latch;
static bool irq_enabled;

// 24C02
enum { EE_IDLE, EE_DEVICE, EE_WORD, EE_WRITE, EE_READ };
static uint8_t eeprom[EEPROM_SIZE];
static uint8_t ee_mode;
static uint8_t ee_bits;  // clocks into the current byte (0-9)
static uint8_t ee_shift; // byte being received
static uint8_t ee_addr;
static bool ee_data_out;    // the byte on the pin is data (read mode): its ninth clock is the master's acknowledge
static bool ee_read_enable; // $800D bit 7: without it a read shows the pin high (as Mesen)
static uint8_t ee_out;      // the data pin
static bool ee_scl;
static bool ee_sda;

static void bandai_update(void)
{
    uint32_t banks16 = prg_rom_size_bytes >> 14;
    prg_slot_offset[0] = ((prg_reg & 0x0Fu) % banks16) << 14;
    prg_slot_offset[1] = (banks16 - 1) << 14;
    uint32_t banks = chr_size_bytes >> 10;
    for (unsigned i = 0; i < 8; i++) {
        chr_slot_offset[i] = chr_is_ram ? (i << 10) : ((chr_reg[i] % banks) << 10);
    }
    for (unsigned nt = 0; nt < 4; nt++) {
        switch (mirroring & 3) {
        case 0: ciram_page[nt] = (uint16_t)((nt & 1) << 10); break;
        case 1: ciram_page[nt] = (uint16_t)((nt >> 1) << 10); break;
        case 2: ciram_page[nt] = 0; break;
        default: ciram_page[nt] = 0x400; break;
        }
    }
}

static void ee_byte_done(void)
{
    uint8_t b = ee_shift;
    bool ack = false;
    switch (ee_mode) {
    case EE_DEVICE:
        if ((b & 0xFE) == 0xA0) {
            ack = true;
            ee_mode = (b & 1) ? EE_READ : EE_WORD;
            ee_data_out = false;
        } else {
            ee_mode = EE_IDLE;
        }
        break;
    case EE_WORD:
        ee_addr = b;
        ack = true;
        ee_mode = EE_WRITE;
        break;
    case EE_WRITE:
        eeprom[ee_addr] = b;
        ee_addr = (uint8_t)((ee_addr & 0xF8) | ((ee_addr + 1) & 7)); // page write: wraps within 8 bytes
        ack = true;
        break;
    default: break;
    }
    ee_out = ack ? 0 : 1;
}

static void ee_write(bool scl, bool sda)
{
    if (ee_scl && scl) {
        if (ee_sda && !sda) { // START
            ee_mode = EE_DEVICE;
            ee_bits = 0;
            ee_shift = 0;
            ee_out = 1;
        } else if (!ee_sda && sda) { // STOP
            ee_mode = EE_IDLE;       // (the pin keeps its last level, as in Mesen)
        }
    } else if (!ee_scl && scl) { // rising edge
        if (ee_mode != EE_IDLE) {
            if (ee_bits < 8) {
                if (ee_mode != EE_READ) {
                    ee_shift = (uint8_t)((ee_shift << 1) | (sda ? 1 : 0));
                }
                ee_bits++;
                if ((8 == ee_bits) && (ee_mode != EE_READ)) {
                    ee_byte_done();
                }
            } else if (8 == ee_bits) { // the ack clock
                ee_bits++;
                if (EE_READ == ee_mode && ee_data_out) {
                    if (sda) { // not acknowledged: the master is done
                        ee_mode = EE_IDLE;
                    } else {
                        ee_addr++;
                    }
                }
            }
        }
    } else if (ee_scl && !scl) { // falling edge: the next bit is put on the pin
        if (ee_mode != EE_IDLE) {
            if (9 == ee_bits) {
                ee_bits = 0;
                ee_shift = 0;
            }
            if (EE_READ == ee_mode) {
                if (ee_bits < 8) {
                    ee_data_out = true;
                    ee_out = (uint8_t)((eeprom[ee_addr] >> (7 - ee_bits)) & 1);
                }
            }
        }
    }
    ee_scl = scl;
    ee_sda = sda;
}

static uint8_t bandai_read_prg(uint16_t address)
{
    if (address >= 0x8000) {
#ifdef MESEN_COMPATIBILITY
        if (!prg_selected && address < 0xC000) {
            return cpu_get_open_bus();
        }
#endif
        return prg_rom_storage[prg_slot_offset[(address >> 14) & 1] | (address & 0x3FFF)];
    }
    if (address >= 0x6000) {
        return (uint8_t)((cpu_get_open_bus() & 0xEF) | ((ee_read_enable ? ee_out : 1) << 4));
    }
    return cpu_get_open_bus();
}

static void bandai_write_prg(uint16_t address, uint8_t value)
{
    if (address < 0x6000) {
        return;
    }
    switch (address & 0x0F) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7: chr_reg[address & 7] = value; break;
    case 8:
        prg_reg = value;
        prg_selected = true;
        break;
    case 9: mirroring = value & 3; break;
    case 0xA:
        irq_enabled = (value & BIT_0) != 0;
        irq_counter = irq_latch;
        cpu_irq_request(false, BIT_2);
        break;
    case 0xB: irq_latch = (uint16_t)((irq_latch & 0xFF00) | value); break;
    case 0xC: irq_latch = (uint16_t)((irq_latch & 0x00FF) | (value << 8)); break;
    case 0xD:
        ee_read_enable = (value & BIT_7) != 0;
        ee_write((value & BIT_5) != 0, (value & BIT_6) != 0);
        break;
    default: break;
    }
    bandai_update();
}

static void bandai_cpu_tick(void)
{
    if (!irq_enabled) {
        return;
    }
    if (0 == irq_counter) {
        cpu_irq_request(true, BIT_2);
    }
    irq_counter--;
}

static uint8_t bandai_read_chr(uint16_t address)
{
    return chr_storage[chr_slot_offset[(address >> 10) & 7] | (address & 0x3FF)];
}

static void bandai_write_chr(uint16_t address, uint8_t value)
{
    if (chr_is_ram) {
        chr_storage[chr_slot_offset[(address >> 10) & 7] | (address & 0x3FF)] = value;
    }
}

static uint16_t bandai_get_ciram_addr(uint16_t addr)
{
    return (uint16_t)(ciram_page[(addr >> 10) & 3] | (addr & 0x3FF));
}

static int32_t bandai_debug_prg_offset(uint16_t address)
{
    return (address < 0x8000) ? -1 : (int32_t)(prg_slot_offset[(address >> 14) & 1] | (address & 0x3FFF));
}

static int32_t bandai_debug_chr_offset(uint16_t address)
{
    return (int32_t)(chr_slot_offset[(address >> 10) & 7] | (address & 0x3FF));
}

static const uint8_t *bandai_debug_chr_data(size_t *size)
{
    *size = chr_size_bytes;
    return chr_storage;
}

// Debugger/trace peek: no side effects; the pins and $6000-$7FFF show as the address high byte
static uint8_t bandai_debug_read_prg(uint16_t address)
{
    if (address < 0x8000) {
        if (address >= 0x6000 && !chr_is_ram) {
            return 0; // Mesen's peek of the register read in the CHR-ROM games (the Datach cartridge shows the address high byte)
        }
        return (uint8_t)(address >> 8);
    }
    if (!prg_selected && address < 0xC000) {
        return (uint8_t)(address >> 8);
    }
    return bandai_read_prg(address);
}

static void bandai_get_sav_data(sav_data_t *sav)
{
    sav->data = eeprom; // still owned by this mapper
    sav->size = EEPROM_SIZE;
}

static void bandai_deinit(void)
{
    free(prg_rom_storage);
    prg_rom_storage = NULL;
    prg_rom_size_bytes = 0;
    free(chr_storage);
    chr_storage = NULL;
    chr_size_bytes = 0;
}

static void bandai_state(state_io *io)
{
    STATE(io, prg_slot_offset);
    STATE(io, chr_slot_offset);
    STATE(io, chr_reg);
    STATE(io, prg_reg);
    STATE(io, prg_selected);
    STATE(io, mirroring);
    STATE(io, ciram_page);
    STATE(io, irq_counter);
    STATE(io, irq_latch);
    STATE(io, irq_enabled);
    STATE(io, eeprom);
    STATE(io, ee_mode);
    STATE(io, ee_bits);
    STATE(io, ee_shift);
    STATE(io, ee_addr);
    STATE(io, ee_data_out);
    STATE(io, ee_read_enable);
    STATE(io, ee_out);
    STATE(io, ee_scl);
    STATE(io, ee_sda);
    if (chr_is_ram) {
        state_bytes(io, "bandai.chr_ram", chr_storage, chr_size_bytes);
    }
    STATE_SKIP(prg_rom_storage); // (the pointers, sizes and the kind of CHR come from the ROM)
    STATE_SKIP(prg_rom_size_bytes);
    STATE_SKIP(chr_storage);
    STATE_SKIP(chr_size_bytes);
    STATE_SKIP(chr_is_ram);
}

bool mapper_bandai_fcg_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data)
{
    assert(ines_file_data != NULL);
    assert(p_mapper_info != NULL);
    const ines_header *h = &ines_file_data->header;
    if (h->prg_16kb_banks < 1) {
        LOG_ERROR(MAPPER, "Bandai FCG needs PRG-ROM");
        return false;
    }
    prg_rom_size_bytes = 16 * 1024 * h->prg_16kb_banks;
    chr_is_ram = (0 == h->chr_8kb_banks);
    chr_size_bytes = chr_is_ram ? 8 * 1024 : 8 * 1024 * h->chr_8kb_banks;
    prg_rom_storage = malloc(prg_rom_size_bytes);
    chr_storage = calloc(1, chr_size_bytes);
    if ((NULL == prg_rom_storage) || (NULL == chr_storage)) {
        LOG_ERROR(MAPPER, "unable to allocate PRG/CHR storage");
        bandai_deinit();
        return false;
    }
    memcpy(prg_rom_storage, ines_file_data->prg_rom, prg_rom_size_bytes);
    if (!chr_is_ram) {
        memcpy(chr_storage, ines_file_data->chr_rom, chr_size_bytes);
    }
    memset(eeprom, 0x00, sizeof(eeprom));
    if (sav_data->size > 0) {
        memcpy(eeprom, sav_data->data, (sav_data->size < EEPROM_SIZE) ? sav_data->size : EEPROM_SIZE);
    }
    memset(chr_reg, 0, sizeof(chr_reg));
    prg_reg = 0;
    prg_selected = false;
    mirroring = (h->flags_6 & BIT_0) ? 0 : 1;
    irq_counter = irq_latch = 0;
    irq_enabled = false;
    ee_mode = EE_IDLE;
    ee_bits = ee_shift = ee_addr = 0;
    ee_out = 1;
    ee_data_out = false;
    ee_read_enable = false;
    ee_scl = ee_sda = false;
    bandai_update();

    p_mapper_info->deinit = bandai_deinit;
    p_mapper_info->read_prg = bandai_read_prg;
    p_mapper_info->write_prg = bandai_write_prg;
    p_mapper_info->read_chr = bandai_read_chr;
    p_mapper_info->write_chr = bandai_write_chr;
    p_mapper_info->get_ciram_addr = bandai_get_ciram_addr;
    p_mapper_info->debug_read_chr = bandai_read_chr;
    p_mapper_info->debug_chr_offset = bandai_debug_chr_offset;
    p_mapper_info->debug_chr_data = bandai_debug_chr_data;
    p_mapper_info->debug_prg_offset = bandai_debug_prg_offset;
    p_mapper_info->debug_read_prg = bandai_debug_read_prg;
    p_mapper_info->get_sav_data = bandai_get_sav_data;
    p_mapper_info->prg_bank_size = 0x4000;
    p_mapper_info->state = bandai_state;
    p_mapper_info->cpu_tick = bandai_cpu_tick;
    return true;
}
