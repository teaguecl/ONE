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

/* NES-EVENT (iNES mapper 105: Nintendo World Championships 1990): an MMC1 with extra logic and a countdown timer. https://www.nesdev.org/wiki/INES_Mapper_105
 *   The MMC1 serial port as usual (control $8000, CHR 0 $A000, CHR 1 $C000, PRG $E000); 8KB of CHR-RAM, 256KB of PRG-ROM in two halves.
 *   CHR 0 bit 3 = 0 (the state after a reset): the first half; PRG is one 32KB bank picked by CHR 0 bits 1-2.
 *   CHR 0 bit 3 = 1: the second half, banked by the PRG register and the control register's PRG mode as on an MMC1 (16KB banks 8-15).
 *   CHR 0 bit 4 = 1 holds the timer at zero and its IRQ off; 0 lets it count CPU cycles; the IRQ comes when the 30-bit counter reaches
 *   $20000000 + (the DIP switches << 25) -- minutes after the game starts. The DIP switches read 0 (as Mesen).
 *   Written from the wiki; it needs a Mesen trace of the cartridge to be checked. */

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
#include "mapper_nes_event.h"
#include "savestate.h"

#define CHR_RAM_SIZE 0x2000u
#define WRAM_SIZE    0x2000u
#define DIP_SWITCHES 0u

static uint8_t *prg_rom_storage = NULL;
static uint32_t prg_rom_size_bytes = 0;
static uint8_t *chr_ram = NULL;
static uint8_t *wram = NULL;        // 8KB at $6000-$7FFF (the PRG register's bit 4 disables it, as on an MMC1)
static uint32_t prg_slot_offset[2]; // 16KB slots
static uint16_t ciram_page[4];

static uint8_t shift_reg;
static uint8_t control_reg;
static uint8_t chr_reg0;
static uint8_t chr_reg1;
static uint8_t prg_reg;
static uint64_t last_write_cycle;
static uint32_t irq_counter;
static bool irq_running;

static void ev_update(void)
{
    uint32_t banks = prg_rom_size_bytes >> 14;
    if (!(chr_reg0 & BIT_3)) {
        // initial state: one 32KB bank of the first half
        uint32_t b = ((chr_reg0 >> 1) & 3u) << 1;
        prg_slot_offset[0] = (b % banks) << 14;
        prg_slot_offset[1] = ((b + 1) % banks) << 14;
    } else {
        // the second half: an MMC1's PRG banking on 16KB banks 8-15
        uint32_t mode = (control_reg >> 2) & 3;
        uint32_t p = prg_reg & 7u;
        uint32_t lo, hi;
        switch (mode) {
        case 2:
            lo = 0;
            hi = p;
            break; // first bank fixed at $8000
        case 3:
            lo = p;
            hi = 7;
            break; // last bank fixed at $C000
        default:
            lo = p & ~1u;
            hi = lo | 1u;
            break; // 32KB
        }
        prg_slot_offset[0] = ((8 + lo) % banks) << 14;
        prg_slot_offset[1] = ((8 + hi) % banks) << 14;
    }
    unsigned m = control_reg & 3;
    for (unsigned nt = 0; nt < 4; nt++) {
        switch (m) {
        case 0: ciram_page[nt] = 0; break;
        case 1: ciram_page[nt] = 0x400; break;
        case 2: ciram_page[nt] = (uint16_t)((nt & 1) << 10); break;
        default: ciram_page[nt] = (uint16_t)((nt >> 1) << 10); break;
        }
    }
    bool run = !(chr_reg0 & BIT_4);
    if (!run) {
        irq_counter = 0;
        cpu_irq_request(false, BIT_2);
    }
    irq_running = run;
}

static void ev_deinit(void)
{
    free(prg_rom_storage);
    prg_rom_storage = NULL;
    prg_rom_size_bytes = 0;
    free(chr_ram);
    chr_ram = NULL;
    free(wram);
    wram = NULL;
}

static uint8_t ev_read_prg(uint16_t address)
{
    if (address >= 0x8000) {
        return prg_rom_storage[prg_slot_offset[(address >> 14) & 1] | (address & 0x3FFF)];
    }
    if ((address >= 0x6000) && !(prg_reg & BIT_4)) {
        return wram[address & (WRAM_SIZE - 1)];
    }
    return cpu_get_open_bus();
}

static void ev_write_prg(uint16_t address, uint8_t value)
{
    if (address < 0x8000) {
        if ((address >= 0x6000) && !(prg_reg & BIT_4)) {
            wram[address & (WRAM_SIZE - 1)] = value;
        }
        return;
    }
    uint64_t now = cpu_get_cycle();
    if (value & BIT_7) {
        shift_reg = 0x10;
        control_reg |= 0x0C;
        ev_update();
    } else if ((now - last_write_cycle) >= 2) {
        if (shift_reg & BIT_0) {
            uint8_t v = (uint8_t)((shift_reg >> 1) | ((value & BIT_0) << 4));
            shift_reg = 0x10;
            switch ((address >> 13) & 3) {
            case 0: control_reg = v; break;
            case 1: chr_reg0 = v; break;
            case 2: chr_reg1 = v; break;
            default: prg_reg = v; break;
            }
            ev_update();
        } else {
            shift_reg = (uint8_t)((shift_reg >> 1) | ((value & BIT_0) << 4));
        }
    }
    last_write_cycle = now;
}

static void ev_cpu_tick(void)
{
    if (!irq_running) {
        return;
    }
    irq_counter++;
    if (irq_counter == (0x20000000u | (DIP_SWITCHES << 25))) {
        cpu_irq_request(true, BIT_2);
    }
}

static uint8_t ev_read_chr(uint16_t address)
{
    assert(address < 0x2000);
    return chr_ram[address];
}

static void ev_write_chr(uint16_t address, uint8_t value)
{
    assert(address < 0x2000);
    chr_ram[address] = value;
}

static uint16_t ev_get_ciram_addr(uint16_t addr)
{
    return ciram_page[(addr >> 10) & 3] | (addr & 0x3FF);
}

static int32_t ev_debug_chr_offset(uint16_t address)
{
    return (int32_t)address;
}

static const uint8_t *ev_debug_chr_data(size_t *size)
{
    *size = CHR_RAM_SIZE;
    return chr_ram;
}

static int32_t ev_debug_prg_offset(uint16_t address)
{
    return (int32_t)(prg_slot_offset[(address >> 14) & 1] | (address & 0x3FFF));
}

static uint8_t ev_debug_read_prg(uint16_t address)
{
    if (address < 0x6000) {
        return (uint8_t)(address >> 8);
    }
    return (address < 0x8000) ? wram[address & (WRAM_SIZE - 1)] : ev_read_prg(address);
}

static uint8_t *ev_debug_prg_ram(size_t *size)
{
    *size = WRAM_SIZE;
    return wram;
}

static void ev_state(state_io *io)
{
    STATE(io, prg_slot_offset);
    STATE(io, ciram_page);
    STATE(io, shift_reg);
    STATE(io, control_reg);
    STATE(io, chr_reg0);
    STATE(io, chr_reg1);
    STATE(io, prg_reg);
    STATE(io, last_write_cycle);
    STATE(io, irq_counter);
    STATE(io, irq_running);
    state_bytes(io, "event.chr_ram", chr_ram, CHR_RAM_SIZE);
    state_bytes(io, "event.wram", wram, WRAM_SIZE);
    STATE_SKIP(wram);
    STATE_SKIP(prg_rom_storage); // (the pointer and size come from the ROM)
    STATE_SKIP(prg_rom_size_bytes);
    STATE_SKIP(chr_ram);
}

bool mapper_nes_event_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    assert(ines_file_data != NULL);
    assert(p_mapper_info != NULL);
    const ines_header *h = &ines_file_data->header;
    if (h->prg_16kb_banks < 16) {
        LOG_ERROR(MAPPER, "NES-EVENT needs 256KB of PRG-ROM");
        return false;
    }
    prg_rom_size_bytes = 16 * 1024 * h->prg_16kb_banks;
    prg_rom_storage = malloc(prg_rom_size_bytes);
    chr_ram = calloc(1, CHR_RAM_SIZE);
    wram = calloc(1, WRAM_SIZE);
    if ((NULL == prg_rom_storage) || (NULL == chr_ram) || (NULL == wram)) {
        LOG_ERROR(MAPPER, "unable to allocate PRG/CHR storage");
        ev_deinit();
        return false;
    }
    memcpy(prg_rom_storage, ines_file_data->prg_rom, prg_rom_size_bytes);

    shift_reg = 0x10;
    control_reg = 0x0C;
    chr_reg0 = chr_reg1 = prg_reg = 0;
    last_write_cycle = 0;
    irq_counter = 0;
    ev_update();

    p_mapper_info->deinit = ev_deinit;
    p_mapper_info->read_prg = ev_read_prg;
    p_mapper_info->write_prg = ev_write_prg;
    p_mapper_info->read_chr = ev_read_chr;
    p_mapper_info->write_chr = ev_write_chr;
    p_mapper_info->get_ciram_addr = ev_get_ciram_addr;
    p_mapper_info->debug_read_chr = ev_read_chr;
    p_mapper_info->debug_chr_offset = ev_debug_chr_offset;
    p_mapper_info->debug_chr_data = ev_debug_chr_data;
    p_mapper_info->debug_prg_offset = ev_debug_prg_offset;
    p_mapper_info->debug_prg_ram = ev_debug_prg_ram;
    p_mapper_info->prg_bank_size = 0x4000;
    p_mapper_info->debug_read_prg = ev_debug_read_prg;
    p_mapper_info->state = ev_state;
    p_mapper_info->cpu_tick = ev_cpu_tick;
    return true;
}
