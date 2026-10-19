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

/* Konami VRC6 board (mapper_vrc6.h). https://www.nesdev.org/wiki/VRC6
 *   PRG: a 16KB bank at $8000 ($8000 register), an 8KB bank at $C000 ($C000), the last 8KB fixed at $E000; 8KB work RAM at $6000
 *   ($B003 bit 7 enables it; Mesen ignores that).
 *   CHR: eight registers ($D000-$D003 R0-3, $E000-$E003 R4-7), $B003 bits 0-1 pick the mode: 0 = eight 1KB banks, 1 = four 2KB banks
 *   (R0-R3), 2 / 3 = four 1KB banks (R0-R3) and two 2KB banks (R4, R5): Mesen's, from tests/vrc6.
 *   Name tables: $B003 bits 0-5 pick, for each of the four, a CIRAM page or (bit 4) a 1KB CHR-ROM bank of R4-R7 (nt_source).
 *   IRQ ($F000 latch, $F001 control, $F002 acknowledge): the VRC4's (a counter of CPU cycles or, with a /3 prescaler, scanlines).
 *   Audio ($9000-$9003, $A000-$A002, $B000-$B002): two 16-step pulse channels with 3-bit duty and 4-bit volume, and a sawtooth that
 *   adds its rate to an accumulator every second step of 14. */

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
#include "mapper_vrc6.h"
#include "rp2a03.h"
#include "savestate.h"

#define WRAM_SIZE 0x2000u

static uint8_t *prg_rom_storage = NULL;
static uint32_t prg_rom_size_bytes = 0;
static uint8_t *chr_storage = NULL; // CHR-ROM, or 8KB of CHR-RAM
static uint32_t chr_size_bytes = 0;
static bool chr_is_ram = false;
static uint8_t *wram = NULL;
static bool swap_lines;             // VRC6b: A0 and A1 exchanged
static uint32_t prg_slot_offset[4]; // 8KB slots
static uint32_t chr_slot_offset[8]; // 1KB slots
static uint16_t ciram_page[4];
static uint32_t nt_chr_offset[4]; // CHR-ROM name tables ($B003 bit 4): the 1KB bank each one shows

static uint8_t
    prg_unmapped;         // bit n: slot n has no PRG bank selected yet (Mesen: open bus until its register is written)
static uint8_t prg16_reg; // $8000
static uint8_t prg8_reg;  // $C000
static uint8_t chr_reg[8];
static uint8_t ppu_mode; // $B003
static uint8_t irq_latch;
static uint8_t irq_counter;
static int16_t irq_prescaler;
static bool irq_enabled;
static bool irq_enable_after_ack;
static bool irq_cycle_mode;

// --- audio -----------------------------------------------------------------------------------------------------------------
typedef struct {
    uint8_t reg0;  // pulse: bit 7 = constant volume, bits 4-6 duty, bits 0-3 volume; saw: bits 0-5 rate
    uint16_t freq; // 12 bits
    bool enabled;
    uint16_t timer;
    uint8_t step; // pulse 0-15, saw 0-13
    uint8_t acc;  // saw accumulator
} vrc6_channel_t;

static vrc6_channel_t chan[3];     // pulse 1, pulse 2, sawtooth
static uint8_t audio_ctrl;         // $9003: bit 0 halt, bits 1-2 frequency shift (4 / 8)
static double audio_pushed = -1.0; // the level the APU mixer has (not saved: the next tick pushes it again)

// Levels in the APU mixer's units (0.0 .. 1.0) per step of a pulse (0-15) and of the sawtooth (0-31)
#define PULSE_LEVEL 0.01479
#define SAW_LEVEL   0.01479

static void vrc6_update_mirroring(void);

static void vrc6_update_prg(void)
{
    uint32_t banks = prg_rom_size_bytes >> 13;
    uint32_t b16 = ((prg16_reg & 0x0Fu) << 1) % banks;
    prg_slot_offset[0] = b16 << 13;
    prg_slot_offset[1] = ((b16 + 1) % banks) << 13;
    prg_slot_offset[2] = ((prg8_reg & 0x1Fu) % banks) << 13;
    prg_slot_offset[3] = (banks - 1) << 13;
}

static void vrc6_update_chr(void)
{
    uint32_t banks = chr_size_bytes >> 10;
    static const uint8_t reg_for_slot[3][8] = {
        {0, 1, 2, 3, 4, 5, 6, 7}, {0, 0, 1, 1, 2, 2, 3, 3}, {0, 1, 2, 3, 4, 4, 5, 5}};
    unsigned mode = ppu_mode & 3;
    for (unsigned slot = 0; slot < 8; slot++) {
        unsigned r = chr_reg[reg_for_slot[mode ? (mode == 1 ? 1 : 2) : 0][slot]];
        bool two_kb = (1 == mode) || ((mode >= 2) && (slot >= 4));
        unsigned bank = two_kb ? ((r & 0xFEu) | (slot & 1u)) : r;
        chr_slot_offset[slot] = (bank % banks) << 10;
    }
    vrc6_update_mirroring(); // (name tables from CHR-ROM follow R4-R7)
}

// Where each name table comes from, by $B003 bits 0-3 and 5 (bits 6 and 7 do not matter; bit 4 only chooses CIRAM or CHR-ROM): the CHR
// register R4-R7 whose bank number it follows, as it is (0), forced even (1) or forced odd (2) -- read off Mesen's traces of
// tests/vrc6 (index: bit 5 << 4 | bits 0-3). With bit 4 set the name table shows that 1KB bank of CHR-ROM; with bit 4 clear it shows the
// CIRAM page given by the low bit of that bank number. So the usual $20 / $24 / $2C (forced even / odd) are vertical / horizontal / one-screen,
// and the other values flip with the low bits of R4-R7.
static const uint8_t nt_source[32][4] = {
    {0x88, 0x88, 0x8C, 0x8C}, // $00
    {0x80, 0x84, 0x88, 0x8C}, // $01
    {0x88, 0x8C, 0x88, 0x8C}, // $02
    {0x88, 0x8C, 0x88, 0x8C}, // $03
    {0x88, 0x8C, 0x88, 0x8C}, // $04
    {0x80, 0x84, 0x88, 0x8C}, // $05
    {0x88, 0x88, 0x8C, 0x8C}, // $06
    {0x88, 0x88, 0x8C, 0x8C}, // $07
    {0x88, 0x88, 0x8C, 0x8C}, // $08
    {0x80, 0x84, 0x88, 0x8C}, // $09
    {0x88, 0x8C, 0x88, 0x8C}, // $0A
    {0x88, 0x8C, 0x88, 0x8C}, // $0B
    {0x88, 0x8C, 0x88, 0x8C}, // $0C
    {0x80, 0x84, 0x88, 0x8C}, // $0D
    {0x88, 0x88, 0x8C, 0x8C}, // $0E
    {0x88, 0x88, 0x8C, 0x8C}, // $0F
    {0x89, 0x8A, 0x8D, 0x8E}, // $20
    {0x80, 0x84, 0x88, 0x8C}, // $21
    {0x88, 0x8C, 0x88, 0x8C}, // $22
    {0x89, 0x8D, 0x8A, 0x8E}, // $23
    {0x89, 0x8D, 0x8A, 0x8E}, // $24
    {0x80, 0x84, 0x88, 0x8C}, // $25
    {0x88, 0x88, 0x8C, 0x8C}, // $26
    {0x89, 0x8A, 0x8D, 0x8E}, // $27
    {0x89, 0x89, 0x8D, 0x8D}, // $28
    {0x80, 0x84, 0x88, 0x8C}, // $29
    {0x88, 0x8C, 0x88, 0x8C}, // $2A
    {0x8A, 0x8E, 0x8A, 0x8E}, // $2B
    {0x8A, 0x8E, 0x8A, 0x8E}, // $2C
    {0x80, 0x84, 0x88, 0x8C}, // $2D
    {0x88, 0x88, 0x8C, 0x8C}, // $2E
    {0x89, 0x89, 0x8D, 0x8D}, // $2F
};

static void vrc6_update_mirroring(void)
{
    uint32_t banks = chr_size_bytes >> 10;
    for (unsigned nt = 0; nt < 4; nt++) {
        uint8_t src = nt_source[((ppu_mode >> 1) & 0x10) | (ppu_mode & 0x0F)][nt];
        unsigned bank = chr_reg[4 + ((src >> 2) & 3)];
        switch (src & 3) {
        case 1: bank &= ~1u; break;
        case 2: bank |= 1u; break;
        default: break;
        }
        if (ppu_mode & BIT_4) {
            ciram_page[nt] = 0x800; // CHR-ROM name table: the PPU asks vrc6_read_nt()
            nt_chr_offset[nt] = (bank % banks) << 10;
        } else {
            ciram_page[nt] = (uint16_t)((bank & 1u) << 10);
        }
    }
}

static void vrc6_deinit(void)
{
    free(prg_rom_storage);
    prg_rom_storage = NULL;
    prg_rom_size_bytes = 0;
    free(chr_storage);
    chr_storage = NULL;
    chr_size_bytes = 0;
    free(wram);
    wram = NULL;
    rp2a03_set_expansion(0.0);
}

// $B003 bit 7 enables the work RAM. Mesen ignores it: Esper Dream 2 clears RAM before ever writing $B003 and Mesen keeps those writes
static bool vrc6_wram_enabled(void)
{
#ifdef MESEN_COMPATIBILITY
    return true;
#else
    return (ppu_mode & BIT_7) != 0;
#endif
}

static uint8_t vrc6_read_prg(uint16_t address)
{
    if (address >= 0x8000) {
        if (prg_unmapped & (1u << ((address >> 13) & 3))) {
            return cpu_get_open_bus();
        }
        return prg_rom_storage[prg_slot_offset[(address >> 13) & 3] | (address & 0x1FFF)];
    }
    if ((address >= 0x6000) && vrc6_wram_enabled()) {
        return wram[address & (WRAM_SIZE - 1)];
    }
    return cpu_get_open_bus();
}

static void vrc6_clock_irq_counter(void)
{
    if (0xFF == irq_counter) {
        irq_counter = irq_latch;
        cpu_irq_request(true, BIT_2);
    } else {
        irq_counter++;
    }
}

static unsigned chan_period(const vrc6_channel_t *c)
{
    unsigned shift = (audio_ctrl & BIT_2) ? 8 : ((audio_ctrl & BIT_1) ? 4 : 0);
    return (unsigned)(c->freq >> shift) + 1;
}

static unsigned pulse_output(const vrc6_channel_t *c)
{
    if (!c->enabled || (audio_ctrl & BIT_0)) {
        return 0;
    }
    if (c->reg0 & BIT_7) {
        return c->reg0 & 0x0Fu;
    }
    return (c->step <= ((c->reg0 >> 4) & 7)) ? (c->reg0 & 0x0Fu) : 0;
}

static unsigned saw_output(const vrc6_channel_t *c)
{
    return (c->enabled && !(audio_ctrl & BIT_0)) ? (unsigned)(c->acc >> 3) : 0;
}

static void vrc6_audio_tick(void)
{
    if (audio_ctrl & BIT_0) {
        return;
    }
    for (unsigned i = 0; i < 3; i++) {
        vrc6_channel_t *c = &chan[i];
        if (!c->enabled) {
            continue;
        }
        if (0 == c->timer) {
            c->timer = (uint16_t)(chan_period(c) - 1);
            if (i < 2) {
                c->step = (c->step + 1) & 0x0F;
            } else {
                c->step = (uint8_t)((c->step + 1) % 14);
                if (0 == c->step) {
                    c->acc = 0;
                } else if (0 == (c->step & 1)) {
                    c->acc = (uint8_t)(c->acc + (c->reg0 & 0x3F));
                }
            }
        } else {
            c->timer--;
        }
    }
}

// Once per CPU cycle
static void vrc6_cpu_tick(void)
{
    vrc6_audio_tick();
    double level = (pulse_output(&chan[0]) + pulse_output(&chan[1])) * PULSE_LEVEL + saw_output(&chan[2]) * SAW_LEVEL;
    if (level != audio_pushed) {
        audio_pushed = level;
        rp2a03_set_expansion(level);
    }
    if (!irq_enabled) {
        return;
    }
    if (irq_cycle_mode) {
        vrc6_clock_irq_counter();
    } else {
        irq_prescaler -= 3;
        if (irq_prescaler <= 0) {
            irq_prescaler += 341;
            vrc6_clock_irq_counter();
        }
    }
}

static void vrc6_audio_write(unsigned channel, unsigned reg, uint8_t value)
{
    vrc6_channel_t *c = &chan[channel];
    switch (reg) {
    case 0: c->reg0 = value; break;
    case 1: c->freq = (uint16_t)((c->freq & 0x0F00) | value); break;
    default:
        c->freq = (uint16_t)((c->freq & 0x00FF) | ((value & 0x0Fu) << 8));
        c->enabled = (value & BIT_7) != 0;
        if (!c->enabled) {
            c->step = 0;
            c->acc = 0;
        }
        break;
    }
}

static void vrc6_write_prg(uint16_t address, uint8_t value)
{
    if ((address >= 0x6000) && (address < 0x8000)) {
        if (vrc6_wram_enabled()) {
            wram[address & (WRAM_SIZE - 1)] = value;
        }
        return;
    }
    if (address < 0x8000) {
        return;
    }
    unsigned reg = address & 3;
    if (swap_lines) {
        reg = ((reg & 1) << 1) | (reg >> 1);
    }
    switch (address & 0xF000) {
    case 0x8000:
        prg16_reg = value & 0x0F;
        prg_unmapped &= (uint8_t)~0x03u;
        vrc6_update_prg();
        break;
    case 0xC000:
        prg8_reg = value & 0x1F;
        prg_unmapped &= (uint8_t)~0x04u;
        vrc6_update_prg();
        break;
    case 0x9000:
        if (3 == reg) {
            audio_ctrl = value;
        } else {
            vrc6_audio_write(0, reg, value);
        }
        break;
    case 0xA000:
        if (reg < 3) {
            vrc6_audio_write(1, reg, value);
        }
        break;
    case 0xB000:
        if (3 == reg) {
            ppu_mode = value;
            vrc6_update_chr();
            vrc6_update_mirroring();
        } else {
            vrc6_audio_write(2, reg, value);
        }
        break;
    case 0xD000:
    case 0xE000:
        chr_reg[(((address >> 12) - 0xD) << 2) | reg] = value;
        vrc6_update_chr();
        break;
    case 0xF000:
        switch (reg) {
        case 0: irq_latch = value; break;
        case 1:
            irq_enable_after_ack = (value & BIT_0) != 0;
            irq_enabled = (value & BIT_1) != 0;
            irq_cycle_mode = (value & BIT_2) != 0;
            if (irq_enabled) {
                irq_counter = irq_latch;
                irq_prescaler = 341;
            }
            cpu_irq_request(false, BIT_2);
            break;
        case 2:
            irq_enabled = irq_enable_after_ack;
            cpu_irq_request(false, BIT_2);
            break;
        default: break;
        }
        break;
    default: break;
    }
}

static uint8_t vrc6_read_chr(uint16_t address)
{
    assert(address < 0x2000);
    return chr_storage[chr_slot_offset[(address >> 10) & 7] | (address & 0x3FF)];
}

static void vrc6_write_chr(uint16_t address, uint8_t value)
{
    assert(address < 0x2000);
    if (chr_is_ram) {
        chr_storage[chr_slot_offset[(address >> 10) & 7] | (address & 0x3FF)] = value;
    }
}

static uint16_t vrc6_get_ciram_addr(uint16_t addr)
{
    return ciram_page[(addr >> 10) & 3] | (addr & 0x3FF);
}

static uint8_t vrc6_read_nt(uint16_t address)
{
    return chr_storage[nt_chr_offset[(address >> 10) & 3] | (address & 0x3FF)];
}

static void vrc6_write_nt(uint16_t address, uint8_t value)
{
    (void)address; // CHR-ROM
    (void)value;
}

static int32_t vrc6_debug_chr_offset(uint16_t address)
{
    return (int32_t)(chr_slot_offset[(address >> 10) & 7] | (address & 0x3FF));
}

static const uint8_t *vrc6_debug_chr_data(size_t *size)
{
    *size = chr_size_bytes;
    return chr_storage;
}

static int32_t vrc6_debug_prg_offset(uint16_t address)
{
    return (int32_t)(prg_slot_offset[(address >> 13) & 3] | (address & 0x1FFF));
}

static uint8_t *vrc6_debug_prg_ram(size_t *size)
{
    *size = WRAM_SIZE;
    return wram;
}

static void vrc6_get_sav_data(sav_data_t *sav)
{
    sav->data = wram; // still owned by this mapper
    sav->size = WRAM_SIZE;
}

// Debugger/trace peek: no side effects; the registers below $6000 show the address high byte
static uint8_t vrc6_debug_read_prg(uint16_t address)
{
    if ((address >= 0x6000) && (address < 0x8000)) {
        return wram[address & (WRAM_SIZE - 1)]; // (Mesen ignores $B003 bit 7: vrc6_wram_enabled())
    }
    if ((address < 0x6000) || ((address >= 0x8000) && (prg_unmapped & (1u << ((address >> 13) & 3))))) {
        return (uint8_t)(address >> 8);
    }
    return vrc6_read_prg(address);
}

static void vrc6_state(state_io *io)
{
    audio_pushed = -1.0; // (a loaded state: the mixer gets the level again at the next tick)
    STATE(io, prg_slot_offset);
    STATE(io, chr_slot_offset);
    STATE(io, ciram_page);
    STATE(io, nt_chr_offset);
    STATE(io, prg_unmapped);
    STATE(io, prg16_reg);
    STATE(io, prg8_reg);
    STATE(io, chr_reg);
    STATE(io, ppu_mode);
    STATE(io, irq_latch);
    STATE(io, irq_counter);
    STATE(io, irq_prescaler);
    STATE(io, irq_enabled);
    STATE(io, irq_enable_after_ack);
    STATE(io, irq_cycle_mode);
    STATE(io, chan);
    STATE(io, audio_ctrl);
    state_bytes(io, "vrc6.wram", wram, WRAM_SIZE);
    if (chr_is_ram) {
        state_bytes(io, "vrc6.chr_ram", chr_storage, chr_size_bytes);
    }
    STATE_SKIP(audio_pushed);
    STATE_SKIP(swap_lines); // (the pointers, sizes, wiring and the kind of CHR come from the ROM)
    STATE_SKIP(prg_rom_storage);
    STATE_SKIP(prg_rom_size_bytes);
    STATE_SKIP(chr_storage);
    STATE_SKIP(chr_size_bytes);
    STATE_SKIP(chr_is_ram);
    STATE_SKIP(wram);
}

bool mapper_vrc6_init(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data, bool swap_a0_a1)
{
    assert(ines_file_data != NULL);
    assert(p_mapper_info != NULL);
    const ines_header *h = &ines_file_data->header;
    if (h->prg_16kb_banks < 1) {
        LOG_ERROR(MAPPER, "VRC6 needs PRG-ROM");
        return false;
    }
    swap_lines = swap_a0_a1;
    prg_rom_size_bytes = 16 * 1024 * h->prg_16kb_banks;
    chr_is_ram = (0 == h->chr_8kb_banks);
    chr_size_bytes = chr_is_ram ? 8 * 1024 : 8 * 1024 * h->chr_8kb_banks;
    prg_rom_storage = malloc(prg_rom_size_bytes);
    chr_storage = calloc(1, chr_size_bytes);
    wram = calloc(1, WRAM_SIZE);
    if ((NULL == prg_rom_storage) || (NULL == chr_storage) || (NULL == wram)) {
        LOG_ERROR(MAPPER, "unable to allocate PRG/CHR storage");
        vrc6_deinit();
        return false;
    }
    memcpy(prg_rom_storage, ines_file_data->prg_rom, prg_rom_size_bytes);
    if (!chr_is_ram) {
        memcpy(chr_storage, ines_file_data->chr_rom, chr_size_bytes);
    }
    if ((h->flags_6 & BIT_1) && (sav_data->size > 0)) { // battery: restore the .sav file
        memcpy(wram, sav_data->data, (sav_data->size < WRAM_SIZE) ? sav_data->size : WRAM_SIZE);
    }

    prg16_reg = prg8_reg = 0;
    prg_unmapped = 0;
#ifdef MESEN_COMPATIBILITY
    prg_unmapped = 0x07; // $8000-$DFFF are open bus until the game selects their banks
#endif
    memset(chr_reg, 0, sizeof(chr_reg));
    ppu_mode = 0;
    irq_latch = irq_counter = 0;
    irq_prescaler = 0;
    irq_enabled = irq_enable_after_ack = irq_cycle_mode = false;
    memset(chan, 0, sizeof(chan));
    audio_ctrl = 0;
    audio_pushed = -1.0;
    vrc6_update_prg();
    vrc6_update_chr();
    vrc6_update_mirroring();
    rp2a03_set_expansion(0.0);

    p_mapper_info->deinit = vrc6_deinit;
    p_mapper_info->read_prg = vrc6_read_prg;
    p_mapper_info->write_prg = vrc6_write_prg;
    p_mapper_info->read_chr = vrc6_read_chr;
    p_mapper_info->write_chr = vrc6_write_chr;
    p_mapper_info->get_ciram_addr = vrc6_get_ciram_addr;
    p_mapper_info->read_nt = vrc6_read_nt;
    p_mapper_info->write_nt = vrc6_write_nt;
    p_mapper_info->debug_read_chr = vrc6_read_chr;
    p_mapper_info->debug_chr_offset = vrc6_debug_chr_offset;
    p_mapper_info->debug_chr_data = vrc6_debug_chr_data;
    p_mapper_info->debug_prg_offset = vrc6_debug_prg_offset;
    p_mapper_info->debug_prg_ram = vrc6_debug_prg_ram;
    p_mapper_info->get_sav_data = vrc6_get_sav_data;
    p_mapper_info->prg_bank_size = 0x2000;
    p_mapper_info->debug_read_prg = vrc6_debug_read_prg;
    p_mapper_info->state = vrc6_state;
    p_mapper_info->cpu_tick = vrc6_cpu_tick;
    return true;
}
