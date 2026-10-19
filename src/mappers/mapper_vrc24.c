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

/* Konami VRC2 / VRC4 board (mapper_vrc24.h). https://www.nesdev.org/wiki/VRC2_and_VRC4
 *   PRG: two 8KB banks ($8000 / $A000 registers); the other two slots hold the last two 8KB banks. VRC4 swap mode ($9002 bit 1)
 *   exchanges the $8000 and $C000 slots.
 *   CHR: eight 1KB banks, each a 9-bit number written as two nibbles ($B000-$E003: low 4 bits, then high 5 bits).
 *   Mirroring ($9000): 0 vertical, 1 horizontal, VRC4 also 2 / 3 = one-screen page 0 / 1.
 *   IRQ (VRC4, $F000-$F003): an 8-bit counter that counts up CPU cycles, or scanlines (a /3 prescaler of 341 per line); it
 *   reloads from the latch and requests an IRQ when it overflows.
 *   The work RAM is on at power-on; a VRC4 switches it with $9002 bit 0 (Wai Wai World 2 turns it off and then reads open bus). */

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
#include "mapper_vrc24.h"
#include "savestate.h"

#define WRAM_SIZE 0x2000u

static const vrc24_board_t *board;
static uint8_t *prg_rom_storage = NULL;
static uint32_t prg_rom_size_bytes = 0;
static uint8_t *chr_storage = NULL; // CHR-ROM, or 8KB of CHR-RAM
static uint32_t chr_size_bytes = 0;
static bool chr_is_ram = false;
static uint8_t *wram = NULL;
static uint32_t prg_slot_offset[4]; // 8KB slots $8000, $A000, $C000, $E000
static uint32_t chr_slot_offset[8]; // 1KB slots
static uint16_t ciram_page[4];

static uint8_t prg_reg[2];
static uint16_t chr_reg[8]; // 9 bits: low nibble | high 5 bits << 4
static uint8_t mirroring;
static bool prg_swap;
static bool wram_enabled; // VRC4: $9002 bit 0 (on at power-on, as Mesen has it)
static uint8_t irq_latch;
static uint8_t irq_counter;
static int16_t irq_prescaler;
static bool irq_enabled;
static bool irq_enable_after_ack;
static bool irq_cycle_mode;

static void vrc24_update_prg(void)
{
    uint32_t banks = prg_rom_size_bytes >> 13;
    uint32_t b0 = (prg_reg[0] % banks) << 13, b1 = (prg_reg[1] % banks) << 13;
    uint32_t second_last = (banks - 2) << 13, last = (banks - 1) << 13;
    prg_slot_offset[0] = prg_swap ? second_last : b0;
    prg_slot_offset[1] = b1;
    prg_slot_offset[2] = prg_swap ? b0 : second_last;
    prg_slot_offset[3] = last;
}

static void vrc24_update_chr(void)
{
    uint32_t banks = chr_size_bytes >> 10;
    for (unsigned i = 0; i < 8; i++) {
        uint32_t bank = board->chr_shift ? (unsigned)chr_reg[i] >> 1 : chr_reg[i];
        chr_slot_offset[i] = (bank % banks) << 10;
    }
}

static void vrc24_update_mirroring(void)
{
    for (unsigned nt = 0; nt < 4; nt++) {
        switch (mirroring) {
        case 0: ciram_page[nt] = (uint16_t)((nt & 1) << 10); break;
        case 1: ciram_page[nt] = (uint16_t)((nt >> 1) << 10); break;
        case 2: ciram_page[nt] = 0; break;
        default: ciram_page[nt] = 0x400; break;
        }
    }
}

static void vrc24_deinit(void)
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

static uint8_t vrc24_read_prg(uint16_t address)
{
    if (address >= 0x8000) {
        return prg_rom_storage[prg_slot_offset[(address >> 13) & 3] | (address & 0x1FFF)];
    }
    if ((address >= 0x6000) && wram_enabled) {
        return wram[address & (WRAM_SIZE - 1)];
    }
    return cpu_get_open_bus();
}

static void vrc24_clock_irq_counter(void)
{
    if (0xFF == irq_counter) {
        irq_counter = irq_latch;
        cpu_irq_request(true, BIT_2);
    } else {
        irq_counter++;
    }
}

// Once per CPU cycle
static void vrc24_cpu_tick(void)
{
    if (!irq_enabled) {
        return;
    }
    if (irq_cycle_mode) {
        vrc24_clock_irq_counter();
    } else {
        irq_prescaler -= 3;
        if (irq_prescaler <= 0) {
            irq_prescaler += 341;
            vrc24_clock_irq_counter();
        }
    }
}

static void vrc24_write_prg(uint16_t address, uint8_t value)
{
    if ((address >= 0x6000) && (address < 0x8000)) {
        if (wram_enabled) {
            wram[address & (WRAM_SIZE - 1)] = value;
        }
        return;
    }
    if (address < 0x8000) {
        return;
    }
    unsigned reg = ((address & board->reg0_lines) ? 1u : 0u) | ((address & board->reg1_lines) ? 2u : 0u);
    switch (address & 0xF000) {
    case 0x8000:
        prg_reg[0] = value & 0x1F;
        vrc24_update_prg();
        break;
    case 0xA000:
        prg_reg[1] = value & 0x1F;
        vrc24_update_prg();
        break;
    case 0x9000:
        if (reg < 2) {
            mirroring = value & (board->vrc4 ? 3 : 1);
            vrc24_update_mirroring();
        } else if (board->vrc4) {
            prg_swap = (value & BIT_1) != 0;
            wram_enabled = (value & BIT_0) != 0;
            vrc24_update_prg();
        }
        break;
    case 0xB000:
    case 0xC000:
    case 0xD000:
    case 0xE000: {
        unsigned index = (((address >> 12) - 0xB) << 1) | (reg >> 1);
        if (reg & 1) {
            chr_reg[index] = (uint16_t)((chr_reg[index] & 0x0F) | ((value & 0x1F) << 4));
        } else {
            chr_reg[index] = (uint16_t)((chr_reg[index] & 0x1F0) | (value & 0x0F));
        }
        vrc24_update_chr();
        break;
    }
    case 0xF000:
        if (!board->vrc4) {
            break;
        }
        switch (reg) {
        case 0: irq_latch = (uint8_t)((irq_latch & 0xF0) | (value & 0x0F)); break;
        case 1: irq_latch = (uint8_t)((irq_latch & 0x0F) | ((value & 0x0F) << 4)); break;
        case 2:
            irq_enable_after_ack = (value & BIT_0) != 0;
            irq_enabled = (value & BIT_1) != 0;
            irq_cycle_mode = (value & BIT_2) != 0;
            if (irq_enabled) {
                irq_counter = irq_latch;
                irq_prescaler = 341;
            }
            cpu_irq_request(false, BIT_2);
            break;
        default:
            irq_enabled = irq_enable_after_ack;
            cpu_irq_request(false, BIT_2);
            break;
        }
        break;
    default: break;
    }
}

static uint8_t vrc24_read_chr(uint16_t address)
{
    assert(address < 0x2000);
    return chr_storage[chr_slot_offset[(address >> 10) & 7] | (address & 0x3FF)];
}

static void vrc24_write_chr(uint16_t address, uint8_t value)
{
    assert(address < 0x2000);
    if (chr_is_ram) {
        chr_storage[chr_slot_offset[(address >> 10) & 7] | (address & 0x3FF)] = value;
    }
}

static uint16_t vrc24_get_ciram_addr(uint16_t addr)
{
    return ciram_page[(addr >> 10) & 3] | (addr & 0x3FF);
}

static int32_t vrc24_debug_chr_offset(uint16_t address)
{
    return (int32_t)(chr_slot_offset[(address >> 10) & 7] | (address & 0x3FF));
}

static const uint8_t *vrc24_debug_chr_data(size_t *size)
{
    *size = chr_size_bytes;
    return chr_storage;
}

static int32_t vrc24_debug_prg_offset(uint16_t address)
{
    return (int32_t)(prg_slot_offset[(address >> 13) & 3] | (address & 0x1FFF));
}

static uint8_t *vrc24_debug_prg_ram(size_t *size)
{
    *size = WRAM_SIZE;
    return wram;
}

static void vrc24_get_sav_data(sav_data_t *sav)
{
    sav->data = wram; // still owned by this mapper
    sav->size = WRAM_SIZE;
}

// Debugger/trace peek: the registers below $6000 show as the address high byte
static uint8_t vrc24_debug_read_prg(uint16_t address)
{
    if ((address < 0x6000) || ((address < 0x8000) && !wram_enabled)) {
        return (uint8_t)(address >> 8);
    }
    return vrc24_read_prg(address);
}

// PPU catch-up needs nothing: the IRQ counts CPU cycles, not PPU events.
static void vrc24_state(state_io *io)
{
    STATE(io, prg_slot_offset);
    STATE(io, chr_slot_offset);
    STATE(io, ciram_page);
    STATE(io, prg_reg);
    STATE(io, chr_reg);
    STATE(io, mirroring);
    STATE(io, prg_swap);
    STATE(io, wram_enabled);
    STATE(io, irq_latch);
    STATE(io, irq_counter);
    STATE(io, irq_prescaler);
    STATE(io, irq_enabled);
    STATE(io, irq_enable_after_ack);
    STATE(io, irq_cycle_mode);
    state_bytes(io, "vrc24.wram", wram, WRAM_SIZE);
    if (chr_is_ram) {
        state_bytes(io, "vrc24.chr_ram", chr_storage, chr_size_bytes);
    }
    // (the pointers, sizes and the kind of CHR come from the ROM)
    STATE_SKIP(board); // NOLINT(bugprone-sizeof-expression)
    STATE_SKIP(prg_rom_storage);
    STATE_SKIP(prg_rom_size_bytes);
    STATE_SKIP(chr_storage);
    STATE_SKIP(chr_size_bytes);
    STATE_SKIP(chr_is_ram);
    STATE_SKIP(wram);
}

unsigned vrc24_submapper(const ines_header *h)
{
    nes2_info_t nes2;
    if (mapper_is_nes2(h) && mapper_nes2_info(h, &nes2)) {
        return nes2.submapper;
    }
    return 0;
}

bool mapper_vrc24_init(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data,
                       const vrc24_board_t *wiring)
{
    assert(ines_file_data != NULL);
    assert(p_mapper_info != NULL);
    const ines_header *h = &ines_file_data->header;
    if (h->prg_16kb_banks < 1) {
        LOG_ERROR(MAPPER, "%s needs PRG-ROM", wiring->name);
        return false;
    }
    board = wiring;
    prg_rom_size_bytes = 16 * 1024 * h->prg_16kb_banks;
    chr_is_ram = (0 == h->chr_8kb_banks);
    chr_size_bytes = chr_is_ram ? 8 * 1024 : 8 * 1024 * h->chr_8kb_banks;
    prg_rom_storage = malloc(prg_rom_size_bytes);
    chr_storage = calloc(1, chr_size_bytes);
    wram = calloc(1, WRAM_SIZE);
    if ((NULL == prg_rom_storage) || (NULL == chr_storage) || (NULL == wram)) {
        LOG_ERROR(MAPPER, "unable to allocate PRG/CHR storage");
        vrc24_deinit();
        return false;
    }
    memcpy(prg_rom_storage, ines_file_data->prg_rom, prg_rom_size_bytes);
    if (!chr_is_ram) {
        memcpy(chr_storage, ines_file_data->chr_rom, chr_size_bytes);
    }
    if ((h->flags_6 & BIT_1) && (sav_data->size > 0)) { // battery: restore the .sav file
        memcpy(wram, sav_data->data, (sav_data->size < WRAM_SIZE) ? sav_data->size : WRAM_SIZE);
    }

    memset(prg_reg, 0, sizeof(prg_reg));
    memset(chr_reg, 0, sizeof(chr_reg));
    prg_swap = false;
    wram_enabled = true;
    mirroring = (h->flags_6 & BIT_0) ? 0 : 1; // until $9000 is written
    irq_latch = irq_counter = 0;
    irq_prescaler = 0;
    irq_enabled = irq_enable_after_ack = irq_cycle_mode = false;
    vrc24_update_prg();
    vrc24_update_chr();
    vrc24_update_mirroring();

    p_mapper_info->deinit = vrc24_deinit;
    p_mapper_info->read_prg = vrc24_read_prg;
    p_mapper_info->write_prg = vrc24_write_prg;
    p_mapper_info->read_chr = vrc24_read_chr;
    p_mapper_info->write_chr = vrc24_write_chr;
    p_mapper_info->get_ciram_addr = vrc24_get_ciram_addr;
    p_mapper_info->debug_read_chr = vrc24_read_chr;
    p_mapper_info->debug_chr_offset = vrc24_debug_chr_offset;
    p_mapper_info->debug_chr_data = vrc24_debug_chr_data;
    p_mapper_info->debug_prg_offset = vrc24_debug_prg_offset;
    p_mapper_info->debug_prg_ram = vrc24_debug_prg_ram;
    p_mapper_info->get_sav_data = vrc24_get_sav_data;
    p_mapper_info->prg_bank_size = 0x2000;
    p_mapper_info->debug_read_prg = vrc24_debug_read_prg;
    p_mapper_info->state = vrc24_state;
    if (board->vrc4) {
        p_mapper_info->cpu_tick = vrc24_cpu_tick;
    }
    return true;
}
