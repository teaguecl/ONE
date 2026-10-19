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

/* Sunsoft FME-7 (iNES mapper 69: Batman - Return of the Joker, Gimmick!, Hebereke). https://www.nesdev.org/wiki/Sunsoft_FME-7
 *   $8000-$9FFF selects a command, $A000-$BFFF writes its parameter:
 *     0-7  the eight 1KB CHR banks
 *     8    $6000-$7FFF: bank (bits 0-5), bit 6 = RAM (else ROM), bit 7 = RAM enabled
 *     9-B  the 8KB PRG banks at $8000, $A000, $C000 ($E000 is the last bank)
 *     C    mirroring: 0 vertical, 1 horizontal, 2 / 3 one-screen page 0 / 1
 *     D    IRQ: bit 0 = IRQ enabled, bit 7 = counter enabled (writing it acknowledges the IRQ)
 *     E F  the 16-bit IRQ counter, low / high byte
 *   The counter counts down once per CPU cycle; the IRQ is requested when it wraps from $0000 to $FFFF.
 *   Sunsoft 5B sound ($C000 selects a register 0-F of a YM2149-style PSG, $E000 writes it; the register select is ignored when
 *   its high nibble is not 0): three tone channels, a noise generator and an envelope generator, summed into the APU's mixer
 *   (rp2a03_set_expansion()). Gimmick! is the only game: it uses tones only, which is all that was compared with Mesen. */

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "bits.h"
#include "cpu.h"
#include "ines.h"
#include "log.h"
#include "mapper.h"
#include "mapper_fme7.h"
#include "rp2a03.h"
#include "savestate.h"

#define WRAM_SIZE 0x2000u

static uint8_t *prg_rom_storage = NULL;
static uint32_t prg_rom_size_bytes = 0;
static uint8_t *chr_storage = NULL;
static uint32_t chr_size_bytes = 0;
static bool chr_is_ram = false;
static uint8_t *wram = NULL;
static uint32_t prg_slot_offset[4]; // 8KB slots $8000-$FFFF
static uint32_t chr_slot_offset[8]; // 1KB slots
static uint16_t ciram_page[4];

// --- Sunsoft 5B sound ----------------------------------------------------------------------------------------------------
static uint8_t psg_select;      // $C000: the register $E000 writes
static bool psg_select_enabled; // false when the high nibble of the $C000 write was not 0: $E000 writes are ignored
static uint8_t psg_reg[16];
static uint8_t psg_divider;      // CPU cycles to the next PSG tick (CPU clock / 16)
static uint16_t tone_counter[3]; // PSG ticks since the output last toggled
static uint8_t tone_out[3];
static uint8_t noise_counter;
static bool noise_phase;        // the noise LFSR shifts every second period
static uint32_t noise_lfsr = 1; // 17 bits
static uint32_t env_counter;    // PSG ticks of the current envelope step
static uint8_t env_pos;         // 0-31 in the current ramp
static bool env_attack;         // ramp up
static bool env_holding;
static uint8_t env_level;        // 0-31
static double psg_pushed = -1.0; // the level the APU mixer has (not saved: the next tick pushes it again)

// Level of one channel at each 4-bit volume, relative to volume 15 (volume v is amplitude step 2v+1; the steps between are the geometric mean of their
// neighbours), measured on Mesen's recording of tests/sunsoft5b/s5bvol.nes (the rms of its 220 Hz square wave per volume 1-15, 3 to 18 frames into each
// 20-frame step, about 3.2 dB per step; the low volumes corrected once for ONE's own output stage so that the same measurement of ONE matches), and PSG_LEVEL that of volume 15 in the APU mixer's units (0.0 .. 1.0).
#define PSG_LEVEL 0.526
static const double psg_volume[16] = {
    0.0,          15.8 / 4799,  39.0 / 4799,  60.5 / 4799,   81.0 / 4799,   135.2 / 4799,  188.7 / 4799,  298.6 / 4799,
    405.9 / 4799, 595.9 / 4799, 838.4 / 4799, 1194.2 / 4799, 1706.2 / 4799, 2412.6 / 4799, 3385.0 / 4799, 1.0};
static double psg_dac[32];

static uint8_t
    prg_unmapped; // bit n: slot n has no PRG bank selected yet (Mesen: open bus until its command is written)
static uint8_t command;
static uint8_t chr_reg[8];
static uint8_t prg_reg[4]; // [0] = $6000 (command 8), [1..3] = $8000, $A000, $C000
static uint8_t mirroring;
static uint16_t irq_counter;
static bool irq_enabled;
static bool irq_counting;

static void fme7_update_prg(void)
{
    uint32_t banks = prg_rom_size_bytes >> 13;
    for (unsigned i = 0; i < 3; i++) {
        prg_slot_offset[i] = ((prg_reg[i + 1] & 0x3Fu) % banks) << 13;
    }
    prg_slot_offset[3] = (banks - 1) << 13;
}

static void fme7_update_chr(void)
{
    uint32_t banks = chr_size_bytes >> 10;
    for (unsigned i = 0; i < 8; i++) {
        chr_slot_offset[i] = (chr_reg[i] % banks) << 10;
    }
}

static void fme7_update_mirroring(void)
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

static void fme7_deinit(void)
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

// $6000-$7FFF: ROM or RAM, as command 8 says; RAM that is off (or ROM of a missing bank) is open bus
static uint8_t fme7_read_prg(uint16_t address)
{
    if (address >= 0x8000) {
        if (prg_unmapped & (1u << ((address >> 13) & 3))) {
            return cpu_get_open_bus();
        }
        return prg_rom_storage[prg_slot_offset[(address >> 13) & 3] | (address & 0x1FFF)];
    }
    if (address >= 0x6000) {
        if (!(prg_reg[0] & BIT_6)) {
            return prg_rom_storage[(((prg_reg[0] & 0x3Fu) % (prg_rom_size_bytes >> 13)) << 13) | (address & 0x1FFF)];
        }
        if (prg_reg[0] & BIT_7) {
            return wram[address & (WRAM_SIZE - 1)];
        }
    }
    return cpu_get_open_bus();
}

static unsigned psg_period(unsigned reg)
{
    unsigned p = psg_reg[reg] | ((psg_reg[reg + 1] & 0x0Fu) << 8);
    return p ? p : 1;
}

static void psg_env_restart(void)
{
    env_pos = 0;
    env_counter = 0;
    env_attack = (psg_reg[13] & BIT_2) != 0;
    env_holding = false;
    env_level = env_attack ? 0 : 31;
}

static void psg_env_step(void)
{
    uint8_t shape = psg_reg[13];
    if (env_holding) {
        return;
    }
    if (++env_pos < 32) {
        env_level = env_attack ? env_pos : (uint8_t)(31 - env_pos);
        return;
    }
    uint8_t end_level = env_attack ? 31 : 0;
    if (!(shape & BIT_3)) { // no continue: one ramp, then silence
        env_holding = true;
        env_level = 0;
    } else if (shape & BIT_0) { // hold the last value (or its opposite if alternating)
        env_holding = true;
        env_level = (shape & BIT_1) ? (uint8_t)(31 - end_level) : end_level;
    } else {
        if (shape & BIT_1) {
            env_attack = !env_attack;
        }
        env_pos = 0;
        env_level = env_attack ? 0 : 31;
    }
}

static void psg_tick(void) // CPU clock / 16
{
    for (unsigned c = 0; c < 3; c++) {
        if (++tone_counter[c] >= psg_period(2 * c)) {
            tone_counter[c] = 0;
            tone_out[c] ^= 1;
        }
    }
    unsigned noise_period = psg_reg[6] & 0x1Fu;
    if (++noise_counter >= (noise_period ? noise_period : 1)) {
        noise_counter = 0;
        noise_phase = !noise_phase;
        if (!noise_phase) {
            uint32_t feedback = (noise_lfsr ^ (noise_lfsr >> 3)) & 1;
            noise_lfsr = (noise_lfsr >> 1) | (feedback << 16);
        }
    }
    unsigned env_period = psg_reg[11] | ((unsigned)psg_reg[12] << 8);
    if (++env_counter >= (env_period ? env_period : 1)) {
        env_counter = 0;
        psg_env_step();
    }
    double level = 0.0;
    for (unsigned c = 0; c < 3; c++) {
        bool tone = tone_out[c] || (psg_reg[7] & (1u << c));
        bool noise = (noise_lfsr & 1) || (psg_reg[7] & (8u << c));
        if (tone && noise) {
            uint8_t amp = psg_reg[8 + c];
            level += psg_dac[(amp & BIT_4) ? env_level : (2u * (amp & 0x0Fu) + ((amp & 0x0F) ? 1u : 0u))];
        }
    }
    level *= PSG_LEVEL;
    if (level != psg_pushed) {
        psg_pushed = level;
        rp2a03_set_expansion(level);
    }
}

// Once per CPU cycle
static void fme7_cpu_tick(void)
{
    if (++psg_divider >= 16) {
        psg_divider = 0;
        psg_tick();
    }
    if (!irq_counting) {
        return;
    }
    irq_counter--;
    if ((0xFFFF == irq_counter) && irq_enabled) {
        cpu_irq_request(true, BIT_2);
    }
}

static void psg_write(uint8_t value)
{
    if (!psg_select_enabled) {
        return;
    }
    static const uint8_t mask[16] = {0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
                                     0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF};
    psg_reg[psg_select] = value & mask[psg_select];
    if (13 == psg_select) {
        psg_env_restart();
    }
}

static void fme7_write_parameter(uint8_t value)
{
    switch (command & 0x0F) {
    case 0x0:
    case 0x1:
    case 0x2:
    case 0x3:
    case 0x4:
    case 0x5:
    case 0x6:
    case 0x7:
        chr_reg[command & 7] = value;
        fme7_update_chr();
        break;
    case 0x8: prg_reg[0] = value; break;
    case 0x9:
    case 0xA:
    case 0xB:
        prg_reg[(command & 0x0F) - 8] = value;
        prg_unmapped &= (uint8_t) ~(1u << ((command & 0x0F) - 9));
        fme7_update_prg();
        break;
    case 0xC:
        mirroring = value & 3;
        fme7_update_mirroring();
        break;
    case 0xD:
        irq_enabled = (value & BIT_0) != 0;
        irq_counting = (value & BIT_7) != 0;
        cpu_irq_request(false, BIT_2);
        break;
    case 0xE: irq_counter = (uint16_t)((irq_counter & 0xFF00) | value); break;
    default: irq_counter = (uint16_t)((irq_counter & 0x00FF) | (value << 8)); break;
    }
}

static void fme7_write_prg(uint16_t address, uint8_t value)
{
    if ((address >= 0x6000) && (address < 0x8000)) {
        if ((prg_reg[0] & (BIT_6 | BIT_7)) == (BIT_6 | BIT_7)) {
            wram[address & (WRAM_SIZE - 1)] = value;
        }
        return;
    }
    switch (address & 0xE000) {
    case 0x8000: command = value & 0x0F; break;
    case 0xA000: fme7_write_parameter(value); break;
    case 0xC000:
        psg_select = value & 0x0F;
        psg_select_enabled = (value & 0xF0) == 0;
        break;
    default: psg_write(value); break; // $E000
    }
}

static uint8_t fme7_read_chr(uint16_t address)
{
    assert(address < 0x2000);
    return chr_storage[chr_slot_offset[(address >> 10) & 7] | (address & 0x3FF)];
}

static void fme7_write_chr(uint16_t address, uint8_t value)
{
    assert(address < 0x2000);
    if (chr_is_ram) {
        chr_storage[chr_slot_offset[(address >> 10) & 7] | (address & 0x3FF)] = value;
    }
}

static uint16_t fme7_get_ciram_addr(uint16_t addr)
{
    return ciram_page[(addr >> 10) & 3] | (addr & 0x3FF);
}

static int32_t fme7_debug_chr_offset(uint16_t address)
{
    return (int32_t)(chr_slot_offset[(address >> 10) & 7] | (address & 0x3FF));
}

static const uint8_t *fme7_debug_chr_data(size_t *size)
{
    *size = chr_size_bytes;
    return chr_storage;
}

static int32_t fme7_debug_prg_offset(uint16_t address)
{
    return (int32_t)(prg_slot_offset[(address >> 13) & 3] | (address & 0x1FFF));
}

static uint8_t *fme7_debug_prg_ram(size_t *size)
{
    *size = WRAM_SIZE;
    return wram;
}

static void fme7_get_sav_data(sav_data_t *sav)
{
    sav->data = wram; // still owned by this mapper
    sav->size = WRAM_SIZE;
}

// Debugger/trace peek: no side effects, and open bus shows as the address high byte
static uint8_t fme7_debug_read_prg(uint16_t address)
{
    if ((address >= 0x8000) && (prg_unmapped & (1u << ((address >> 13) & 3)))) {
        return (uint8_t)(address >> 8);
    }
    if ((address >= 0x6000) && (address < 0x8000) && (prg_reg[0] & BIT_6) && !(prg_reg[0] & BIT_7)) {
        return (uint8_t)(address >> 8);
    }
    return (address < 0x6000) ? (uint8_t)(address >> 8) : fme7_read_prg(address);
}

static void fme7_state(state_io *io)
{
    psg_pushed = -1.0; // (a loaded state: the mixer gets the level again at the next tick)
    STATE(io, prg_slot_offset);
    STATE(io, chr_slot_offset);
    STATE(io, ciram_page);
    STATE(io, psg_select);
    STATE(io, psg_select_enabled);
    STATE(io, psg_reg);
    STATE(io, psg_divider);
    STATE(io, tone_counter);
    STATE(io, tone_out);
    STATE(io, noise_counter);
    STATE(io, noise_phase);
    STATE(io, noise_lfsr);
    STATE(io, env_counter);
    STATE(io, env_pos);
    STATE(io, env_attack);
    STATE(io, env_holding);
    STATE(io, env_level);
    STATE_SKIP(psg_pushed);
    STATE_SKIP(psg_dac);
    STATE(io, prg_unmapped);
    STATE(io, command);
    STATE(io, chr_reg);
    STATE(io, prg_reg);
    STATE(io, mirroring);
    STATE(io, irq_counter);
    STATE(io, irq_enabled);
    STATE(io, irq_counting);
    state_bytes(io, "fme7.wram", wram, WRAM_SIZE);
    if (chr_is_ram) {
        state_bytes(io, "fme7.chr_ram", chr_storage, chr_size_bytes);
    }
    STATE_SKIP(prg_rom_storage); // (the pointers, sizes and the kind of CHR come from the ROM)
    STATE_SKIP(prg_rom_size_bytes);
    STATE_SKIP(chr_storage);
    STATE_SKIP(chr_size_bytes);
    STATE_SKIP(chr_is_ram);
    STATE_SKIP(wram);
}

bool mapper_fme7_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data)
{
    assert(ines_file_data != NULL);
    assert(p_mapper_info != NULL);
    const ines_header *h = &ines_file_data->header;
    if (h->prg_16kb_banks < 1) {
        LOG_ERROR(MAPPER, "FME-7 needs PRG-ROM");
        return false;
    }
    prg_rom_size_bytes = 16 * 1024 * h->prg_16kb_banks;
    chr_is_ram = (0 == h->chr_8kb_banks);
    chr_size_bytes = chr_is_ram ? 8 * 1024 : 8 * 1024 * h->chr_8kb_banks;
    prg_rom_storage = malloc(prg_rom_size_bytes);
    chr_storage = calloc(1, chr_size_bytes);
    wram = calloc(1, WRAM_SIZE);
    if ((NULL == prg_rom_storage) || (NULL == chr_storage) || (NULL == wram)) {
        LOG_ERROR(MAPPER, "unable to allocate PRG/CHR storage");
        fme7_deinit();
        return false;
    }
    memcpy(prg_rom_storage, ines_file_data->prg_rom, prg_rom_size_bytes);
    if (!chr_is_ram) {
        memcpy(chr_storage, ines_file_data->chr_rom, chr_size_bytes);
    }
    if ((h->flags_6 & BIT_1) && (sav_data->size > 0)) { // battery: restore the .sav file
        memcpy(wram, sav_data->data, (sav_data->size < WRAM_SIZE) ? sav_data->size : WRAM_SIZE);
    }

    command = 0;
    memset(chr_reg, 0, sizeof(chr_reg));
    memset(prg_reg, 0, sizeof(prg_reg));
    mirroring = (h->flags_6 & BIT_0) ? 0 : 1; // until command C is written
    prg_unmapped = 0;
#ifdef MESEN_COMPATIBILITY
    prg_unmapped = 0x07; // $8000-$DFFF are open bus until the game selects their banks
#endif
    irq_counter = 0;
    irq_enabled = irq_counting = false;
    memset(psg_reg, 0, sizeof(psg_reg));
    memset(tone_counter, 0, sizeof(tone_counter));
    memset(tone_out, 0, sizeof(tone_out));
    psg_select = psg_divider = noise_counter = env_pos = env_level = 0;
    psg_select_enabled = noise_phase = env_attack = env_holding = false;
    noise_lfsr = 1;
    env_counter = 0;
    psg_pushed = -1.0;
    psg_dac[0] = 0.0;
    for (unsigned v = 1; v < 16; v++) {
        psg_dac[2 * v + 1] = psg_volume[v];
        psg_dac[2 * v] = (v > 1) ? sqrt(psg_volume[v - 1] * psg_volume[v]) : psg_volume[1] / 1.2;
    }
    rp2a03_set_expansion(0.0);
    fme7_update_prg();
    fme7_update_chr();
    fme7_update_mirroring();

    p_mapper_info->deinit = fme7_deinit;
    p_mapper_info->read_prg = fme7_read_prg;
    p_mapper_info->write_prg = fme7_write_prg;
    p_mapper_info->read_chr = fme7_read_chr;
    p_mapper_info->write_chr = fme7_write_chr;
    p_mapper_info->get_ciram_addr = fme7_get_ciram_addr;
    p_mapper_info->debug_read_chr = fme7_read_chr;
    p_mapper_info->debug_chr_offset = fme7_debug_chr_offset;
    p_mapper_info->debug_chr_data = fme7_debug_chr_data;
    p_mapper_info->debug_prg_offset = fme7_debug_prg_offset;
    p_mapper_info->debug_prg_ram = fme7_debug_prg_ram;
    p_mapper_info->get_sav_data = fme7_get_sav_data;
    p_mapper_info->prg_bank_size = 0x2000;
    p_mapper_info->debug_read_prg = fme7_debug_read_prg;
    p_mapper_info->state = fme7_state;
    p_mapper_info->cpu_tick = fme7_cpu_tick;
    return true;
}
