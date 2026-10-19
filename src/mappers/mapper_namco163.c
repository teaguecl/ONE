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

/* Namco 163 (iNES mapper 19: Rolling Thunder, Final Lap, Megami Tensei II, Mindseeker...). https://www.nesdev.org/wiki/Namco_163
 *   PRG: three 8KB banks ($E000, $E800, $F000 registers select the ones at $8000, $A000, $C000), the last 8KB fixed at $E000.
 *   CHR: eight 1KB banks ($8000-$BFFF, one register per $800); name tables: four registers at $C000-$DFFF, a value below $E0 shows that
 *   1KB bank of CHR-ROM, $E0 and above the CIRAM page (value & 1).
 *   $4800-$4FFF: the chip's 128-byte RAM (waveforms and the audio channels' registers), through the address in $F800 (bits 0-6; bit 7
 *   increments it after each access). $5000-$5FFF: a 15-bit IRQ counter of CPU cycles, bit 15 of the high byte ($5800) enables it;
 *   it counts up and requests an IRQ when it reaches $7FFF.
 *   Audio: the 8 channels' registers are in the chip RAM ($40-$7F); one channel is updated every 15 CPU cycles. A game can read the phase
 *   registers back, so the update timing shows in a CPU trace.
 *   CHR banks $E0 and above show a page of the console's name table RAM (bank & 1) as pattern data, readable and writable, unless
 *   $E800 bit 6 ($0000-$0FFF) / bit 7 ($1000-$1FFF) is set (tests/namco163). */

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
#include "mapper_namco163.h"
#include "ppu.h"
#include "rp2a03.h"
#include "savestate.h"

#define WRAM_SIZE 0x2000u

static uint8_t *prg_rom_storage = NULL;
static uint32_t prg_rom_size_bytes = 0;
static uint8_t *chr_storage = NULL;
static uint32_t chr_size_bytes = 0;
static uint8_t *wram = NULL;
static uint32_t prg_slot_offset[4];
static uint32_t chr_slot_offset[8];
static int8_t
    chr_ciram_page[8]; // -1: the slot shows CHR-ROM; 0 / 1: it shows that page of the console's name table RAM
static uint16_t ciram_page[4];
static uint32_t nt_chr_offset[4];

static uint8_t
    prg_unmapped; // bit n: slot n has no PRG bank selected yet (Mesen: open bus until its register is written)
static uint8_t prg_reg[3];
static uint8_t chr_reg[8];
static uint8_t nt_reg[4];
static uint8_t chip_ram[128];
static uint8_t chip_addr;    // $F800 (bit 7 = auto-increment)
static uint8_t e800_flags;   // $E800 bits 6-7 (CHR banks $E0+ as CIRAM: not done)
static uint16_t irq_counter; // bit 15 = enabled

// --- audio: eight wavetable channels share one engine that updates one channel every 15 CPU cycles ----------------------------
static bool sound_disabled;   // $E000 bit 6
static uint8_t audio_divider; // CPU cycles into the 15
static uint8_t audio_channel; // the channel the next update is for (counts down from 7)
static int16_t channel_output[8];
static double audio_pushed = -1.0; // the level the APU mixer has (not saved: the next tick pushes it again)

// Level of one unit of (sample - 8) * volume in the APU mixer's units: to be fitted to Mesen's recording
#define N163_LEVEL 0.00098

static void n163_update_prg(void)
{
    uint32_t banks = prg_rom_size_bytes >> 13;
    for (unsigned i = 0; i < 3; i++) {
        prg_slot_offset[i] = ((prg_reg[i] & 0x3Fu) % banks) << 13;
    }
    prg_slot_offset[3] = (banks - 1) << 13;
}

static void n163_update_chr(void)
{
    uint32_t banks = chr_size_bytes >> 10;
    for (unsigned i = 0; i < 8; i++) {
        chr_slot_offset[i] = (chr_reg[i] % banks) << 10;
        // banks $E0 and above are the CIRAM page (bank & 1), unless $E800 bit 6 / 7 (for $0000-$0FFF / $1000-$1FFF) says CHR-ROM
        bool rom_only = (e800_flags & ((i < 4) ? BIT_6 : BIT_7)) != 0;
        chr_ciram_page[i] = ((chr_reg[i] >= 0xE0) && !rom_only) ? (int8_t)(chr_reg[i] & 1) : -1;
    }
}

static void n163_update_nt(void)
{
    uint32_t banks = chr_size_bytes >> 10;
    for (unsigned nt = 0; nt < 4; nt++) {
        if (nt_reg[nt] >= 0xE0) {
            ciram_page[nt] = (uint16_t)((nt_reg[nt] & 1u) << 10);
        } else {
            ciram_page[nt] = 0x800; // CHR-ROM name table: the PPU asks n163_read_nt()
            nt_chr_offset[nt] = (nt_reg[nt] % banks) << 10;
        }
    }
}

static void n163_deinit(void)
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

static uint8_t n163_read_prg(uint16_t address)
{
    if (address >= 0x8000) {
        if (prg_unmapped & (1u << ((address >> 13) & 3))) {
            return cpu_get_open_bus();
        }
        return prg_rom_storage[prg_slot_offset[(address >> 13) & 3] | (address & 0x1FFF)];
    }
    if (address >= 0x6000) {
        return wram[address & (WRAM_SIZE - 1)];
    }
    return cpu_get_open_bus();
}

// $4800-$5FFF reads: the chip RAM (and its address increment), the IRQ counter
static bool n163_read_exp(uint16_t address, uint8_t *value)
{
    switch (address & 0xF800) {
    case 0x4800:
        *value = chip_ram[chip_addr & 0x7F];
        if (chip_addr & BIT_7) {
            chip_addr = (uint8_t)(BIT_7 | ((chip_addr + 1) & 0x7F));
        }
        return true;
    case 0x5000: *value = (uint8_t)irq_counter; return true;
    case 0x5800: *value = (uint8_t)(irq_counter >> 8); return true;
    default: return false;
    }
}

// The trace peek: Mesen's debugger shows the address high byte for these registers
// NOLINTNEXTLINE(readability-non-const-parameter): the signature is mapper_info_t::debug_read_exp
static bool n163_debug_read_exp(uint16_t address, uint8_t *value)
{
    (void)address;
    (void)value;
    return false;
}

static unsigned n163_channels(void)
{
    return ((chip_ram[0x7F] >> 4) & 7u) + 1u;
}

// One update of channel C: its phase advances by its frequency, the waveform sample under the new phase times its volume is its output
static void n163_update_channel(unsigned c)
{
    uint8_t *regs = &chip_ram[0x40 + c * 8];
    uint32_t phase = ((uint32_t)regs[5] << 16) | ((uint32_t)regs[3] << 8) | regs[1];
    uint32_t freq = ((uint32_t)(regs[4] & 3) << 16) | ((uint32_t)regs[2] << 8) | regs[0];
    uint32_t length = 256u - (regs[4] & 0xFCu);
    unsigned offset = regs[6];
    unsigned volume = regs[7] & 0x0Fu;
    phase = (phase + freq) % (length << 16);
    unsigned pos = (phase >> 16) + offset;
    uint8_t byte = chip_ram[(pos >> 1) & 0x7F];
    int sample = (pos & 1) ? (byte >> 4) : (byte & 0x0F);
    channel_output[c] = (int16_t)((sample - 8) * (int)volume);
    regs[5] = (uint8_t)(phase >> 16);
    regs[3] = (uint8_t)(phase >> 8);
    regs[1] = (uint8_t)phase;
}

static void n163_audio_tick(void)
{
    if (sound_disabled) {
        return;
    }
    if (++audio_divider < 15) {
        return;
    }
    audio_divider = 0;
    unsigned count = n163_channels();
    n163_update_channel(audio_channel);
    if (audio_channel <= 8 - count) {
        audio_channel = 7;
    } else {
        audio_channel--;
    }
    int sum = 0;
    for (unsigned c = 8 - count; c < 8; c++) {
        sum += channel_output[c];
    }
    double level = sum * N163_LEVEL;
    if (level != audio_pushed) {
        audio_pushed = level;
        rp2a03_set_expansion(level);
    }
}

// Once per CPU cycle
static void n163_cpu_tick(void)
{
    n163_audio_tick();
    if ((irq_counter & 0x8000) && ((irq_counter & 0x7FFF) != 0x7FFF)) {
        irq_counter++;
        if ((irq_counter & 0x7FFF) == 0x7FFF) {
            cpu_irq_request(true, BIT_2);
        }
    }
}

static void n163_write_prg(uint16_t address, uint8_t value)
{
    if ((address >= 0x6000) && (address < 0x8000)) {
        wram[address & (WRAM_SIZE - 1)] = value;
        return;
    }
    switch (address & 0xF800) {
    case 0x4800:
        chip_ram[chip_addr & 0x7F] = value;
        if (chip_addr & BIT_7) {
            chip_addr = (uint8_t)(BIT_7 | ((chip_addr + 1) & 0x7F));
        }
        break;
    case 0x5000:
        irq_counter = (uint16_t)((irq_counter & 0xFF00) | value);
        cpu_irq_request(false, BIT_2);
        break;
    case 0x5800:
        irq_counter = (uint16_t)((irq_counter & 0x00FF) | (value << 8));
        cpu_irq_request(false, BIT_2);
        break;
    case 0x8000:
    case 0x8800:
    case 0x9000:
    case 0x9800:
    case 0xA000:
    case 0xA800:
    case 0xB000:
    case 0xB800:
        chr_reg[(address >> 11) & 7] = value;
        n163_update_chr();
        break;
    case 0xC000:
    case 0xC800:
    case 0xD000:
    case 0xD800:
        nt_reg[(address >> 11) & 3] = value;
        n163_update_nt();
        break;
    case 0xE000:
        prg_reg[0] = value & 0x3F;
        sound_disabled = (value & BIT_6) != 0;
        prg_unmapped &= (uint8_t)~1u;
        n163_update_prg();
        break;
    case 0xE800:
        prg_reg[1] = value & 0x3F;
        prg_unmapped &= (uint8_t)~2u;
        e800_flags = value & 0xC0;
        n163_update_prg();
        n163_update_chr();
        break;
    case 0xF000:
        prg_reg[2] = value & 0x3F;
        prg_unmapped &= (uint8_t)~4u;
        n163_update_prg();
        break;
    case 0xF800: chip_addr = value; break;
    default: break;
    }
}

static uint8_t n163_read_chr(uint16_t address)
{
    assert(address < 0x2000);
    unsigned slot = (address >> 10) & 7;
    if (chr_ciram_page[slot] >= 0) {
        return ppu_ciram_read(((unsigned)chr_ciram_page[slot] << 10) | (address & 0x3FF));
    }
    return chr_storage[chr_slot_offset[slot] | (address & 0x3FF)];
}

static void n163_write_chr(uint16_t address, uint8_t value)
{
    assert(address < 0x2000);
    unsigned slot = (address >> 10) & 7;
    if (chr_ciram_page[slot] >= 0) {
        ppu_ciram_write(((unsigned)chr_ciram_page[slot] << 10) | (address & 0x3FF), value);
    } // else CHR-ROM
}

static uint16_t n163_get_ciram_addr(uint16_t addr)
{
    return ciram_page[(addr >> 10) & 3] | (addr & 0x3FF);
}

static uint8_t n163_read_nt(uint16_t address)
{
    return chr_storage[nt_chr_offset[(address >> 10) & 3] | (address & 0x3FF)];
}

static void n163_write_nt(uint16_t address, uint8_t value)
{
    (void)address; // CHR-ROM
    (void)value;
}

static int32_t n163_debug_chr_offset(uint16_t address)
{
    if (chr_ciram_page[(address >> 10) & 7] >= 0) {
        return -1; // CIRAM
    }
    return (int32_t)(chr_slot_offset[(address >> 10) & 7] | (address & 0x3FF));
}

static const uint8_t *n163_debug_chr_data(size_t *size)
{
    *size = chr_size_bytes;
    return chr_storage;
}

static int32_t n163_debug_prg_offset(uint16_t address)
{
    return (int32_t)(prg_slot_offset[(address >> 13) & 3] | (address & 0x1FFF));
}

static uint8_t *n163_debug_prg_ram(size_t *size)
{
    *size = WRAM_SIZE;
    return wram;
}

static void n163_get_sav_data(sav_data_t *sav)
{
    sav->data = wram; // still owned by this mapper
    sav->size = WRAM_SIZE;
}

// Debugger/trace peek: no side effects; below $6000 the registers show the address high byte
static uint8_t n163_debug_read_prg(uint16_t address)
{
    if ((address < 0x6000) || ((address >= 0x8000) && (prg_unmapped & (1u << ((address >> 13) & 3))))) {
        return (uint8_t)(address >> 8);
    }
    return n163_read_prg(address);
}

static void n163_state(state_io *io)
{
    STATE(io, prg_slot_offset);
    STATE(io, chr_slot_offset);
    STATE(io, chr_ciram_page);
    STATE(io, ciram_page);
    STATE(io, nt_chr_offset);
    STATE(io, prg_unmapped);
    STATE(io, prg_reg);
    STATE(io, chr_reg);
    STATE(io, nt_reg);
    STATE(io, chip_ram);
    STATE(io, chip_addr);
    STATE(io, e800_flags);
    STATE(io, irq_counter);
    STATE(io, sound_disabled);
    STATE(io, audio_divider);
    STATE(io, audio_channel);
    STATE(io, channel_output);
    audio_pushed = -1.0; // (a loaded state: the mixer gets the level again at the next tick)
    STATE_SKIP(audio_pushed);
    state_bytes(io, "n163.wram", wram, WRAM_SIZE);
    STATE_SKIP(prg_rom_storage); // (the pointers and sizes come from the ROM)
    STATE_SKIP(prg_rom_size_bytes);
    STATE_SKIP(chr_storage);
    STATE_SKIP(chr_size_bytes);
    STATE_SKIP(wram);
}

bool mapper_namco163_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data)
{
    assert(ines_file_data != NULL);
    assert(p_mapper_info != NULL);
    const ines_header *h = &ines_file_data->header;
    if ((h->prg_16kb_banks < 1) || (0 == h->chr_8kb_banks)) {
        LOG_ERROR(MAPPER, "Namco 163 needs PRG-ROM and CHR-ROM");
        return false;
    }
    prg_rom_size_bytes = 16 * 1024 * h->prg_16kb_banks;
    chr_size_bytes = 8 * 1024 * h->chr_8kb_banks;
    prg_rom_storage = malloc(prg_rom_size_bytes);
    chr_storage = malloc(chr_size_bytes);
    wram = calloc(1, WRAM_SIZE);
    if ((NULL == prg_rom_storage) || (NULL == chr_storage) || (NULL == wram)) {
        LOG_ERROR(MAPPER, "unable to allocate PRG/CHR storage");
        n163_deinit();
        return false;
    }
    memcpy(prg_rom_storage, ines_file_data->prg_rom, prg_rom_size_bytes);
    memcpy(chr_storage, ines_file_data->chr_rom, chr_size_bytes);
    if ((h->flags_6 & BIT_1) && (sav_data->size > 0)) { // battery: restore the .sav file
        memcpy(wram, sav_data->data, (sav_data->size < WRAM_SIZE) ? sav_data->size : WRAM_SIZE);
    }

    memset(prg_reg, 0, sizeof(prg_reg));
    prg_unmapped = 0;
#ifdef MESEN_COMPATIBILITY
    prg_unmapped = 0x07; // $8000-$DFFF are open bus until the game selects their banks
#endif
    memset(chr_reg, 0, sizeof(chr_reg));
    memset(nt_reg, 0, sizeof(nt_reg));
    memset(chip_ram, 0, sizeof(chip_ram));
    chip_addr = 0;
    e800_flags = 0;
    irq_counter = 0;
    sound_disabled = false;
    audio_divider = 0;
    audio_channel = 7;
    memset(channel_output, 0, sizeof(channel_output));
    audio_pushed = -1.0;
    rp2a03_set_expansion(0.0);
    n163_update_prg();
    n163_update_chr();
    n163_update_nt();

    p_mapper_info->deinit = n163_deinit;
    p_mapper_info->read_prg = n163_read_prg;
    p_mapper_info->write_prg = n163_write_prg;
    p_mapper_info->read_chr = n163_read_chr;
    p_mapper_info->write_chr = n163_write_chr;
    p_mapper_info->get_ciram_addr = n163_get_ciram_addr;
    p_mapper_info->read_nt = n163_read_nt;
    p_mapper_info->write_nt = n163_write_nt;
    p_mapper_info->read_exp = n163_read_exp;
    p_mapper_info->debug_read_exp = n163_debug_read_exp;
    p_mapper_info->debug_read_chr = n163_read_chr;
    p_mapper_info->debug_chr_offset = n163_debug_chr_offset;
    p_mapper_info->debug_chr_data = n163_debug_chr_data;
    p_mapper_info->debug_prg_offset = n163_debug_prg_offset;
    p_mapper_info->debug_prg_ram = n163_debug_prg_ram;
    p_mapper_info->get_sav_data = n163_get_sav_data;
    p_mapper_info->prg_bank_size = 0x2000;
    p_mapper_info->debug_read_prg = n163_debug_read_prg;
    p_mapper_info->state = n163_state;
    p_mapper_info->cpu_tick = n163_cpu_tick;
    return true;
}
