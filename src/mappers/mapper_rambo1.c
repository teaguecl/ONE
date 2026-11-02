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

/* Tengen RAMBO-1 (337006, iNES mapper 64: Klax, Rolling Thunder, Shinobi, Skull & Crossbones, Xybots, Gauntlet, Road Runner,
 * Hard Drivin'). Written from Chris's own measurements of one chip (the RAMBO-1 emulator guide, datasheet and findings F-001 to
 * F-078; the finding numbers in the comments are his), not from another emulator.
 *
 * The chip is modeled as it is: a state machine that sees CPU writes and, once per M2 cycle, the level of PPU A12 at the M2
 * falling edge. In each CPU cycle the writes come first, then cycle() (the cpu_tick hook, called at the end of the cycle).
 *   Registers: decoded on CPU A0, A13, A14 only (/ROMSEL low); no bus conflict.
 *     $8000 bank select (index 3-0, K bit 5: 1 KB CHR mode, P bit 6: PRG mode, C bit 7: CHR inversion), $8001 data
 *     $A000 bit 0: mirroring (0 vertical, 1 horizontal), $A001 nothing
 *     $C000 IRQ latch, $C001 bit 0: 1 = CPU-cycle mode (every write restarts the counter)
 *     $E000 release /IRQ and disable, $E001 enable
 *   Power on: /IRQ asserted, all PRG windows bank $1F, CHR 1 KB layout with every window bank 0, vertical mirroring, latch $FF.
 *   The console's RESET button does not touch the mapper (this file has no reset hook). */

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
#include "mapper_rambo1.h"
#include "savestate.h"

#define A12_FILTER_FULL 16 // M2 falls with A12 low before a rise counts (F-022, F-051)
#define A12_FILTER_HELD 17 // "above full": set by a $C001 write while full, the next rise does not clock (F-052)

static uint8_t *prg_rom_storage = NULL;
static uint32_t prg_rom_size_bytes = 0;
static uint8_t *chr_storage = NULL;
static uint32_t chr_size_bytes = 0;
static bool chr_is_ram = false;

// --- the chip -------------------------------------------------------------------------------------------------------
static uint8_t ctrl;      // the last $8000 value: K, P, C and the index take effect at once (F-040)
static uint8_t reg[16];   // R0-R5, R8, R9: 8 bits; R6, R7, RF: 5 bits; the others are never used
static uint8_t mirror;    // 0 vertical, 1 horizontal
static uint8_t latch;     // $C000, shared by both counter modes
static bool enabled;      // $E001 sets, $E000 clears
static bool irq;          // /IRQ asserted; sticky until $E000
static bool cpu_mode;     // $C001 bit 0
static uint8_t filt;      // A12 filter: M2 falls with A12 low, 0-16; 17 after a $C001 write while full
static bool reload;       // A12 mode: the next counted pulse reloads the counter
static uint8_t counter;   // A12 mode counter
static uint8_t assert_in; // A12 mode: M2 falls until /IRQ asserts (2 after the clocking fall: F-050)
static bool timer_on;     // CPU mode: the interval timer runs
static int32_t t;         // CPU mode: M2 cycles since the cycle that wrote $C001
static bool next_valid;   // CPU mode: next is set (5 cycles after the $C001 write)
static int32_t next;      // CPU mode: t of the next expiry
static bool a12_high;     // PPU A12 as the PPU last reported it

static void set_irq(bool on)
{
    if (irq != on) {
        irq = on;
        cpu_irq_request(on, BIT_2);
    }
}

static void chip_power_on(void)
{
    ctrl = BIT_5; // K = 1: the 1 KB CHR layout
    memset(reg, 0, sizeof(reg));
    reg[6] = reg[7] = reg[15] = 0x1F;
    mirror = 0;
    latch = 0xFF;
    enabled = false;
    irq = false;
    cpu_mode = false;
    filt = A12_FILTER_FULL; // (the power-on value was not measured: full is assumed)
    reload = true;
    counter = 0;
    assert_in = 0;
    timer_on = false;
    t = 0;
    next_valid = false;
    next = 0;
    a12_high = false;
    set_irq(true); // F-001: /IRQ is asserted until the first $E000
}

// --- banking --------------------------------------------------------------------------------------------------------
static uint32_t prg_offset(uint16_t address)
{
    static const uint8_t order[2][3] = {{6, 7, 15},
                                        {15, 7, 6}}; // P = 0, P = 1; the fourth window is the fixed bank $1F
    uint32_t window = (address >> 13) & 3;
    uint32_t bank = (window == 3) ? 0x1F : reg[order[(ctrl & BIT_6) ? 1 : 0][window]];
    return ((bank % (prg_rom_size_bytes >> 13)) << 13) | (address & 0x1FFF);
}

// The 1 KB CHR bank (CHR A17-A10) for a PPU address in $0000-$1FFF (F-006, F-007)
static uint32_t chr_offset(uint16_t address)
{
    uint32_t window = (address >> 10) & 7;
    uint32_t bank;
    if (ctrl & BIT_7) {
        window ^= 4; // C: the two 4 KB halves swap
    }
    if (window >= 4) {
        bank = reg[window - 2]; // R2-R5: register bit 0 goes to CHR A10 as it is
    } else if (ctrl & BIT_5) {
        static const uint8_t k1[4] = {0, 8, 1, 9};
        bank = reg[k1[window]];
    } else {
        bank = (reg[(window < 2) ? 0 : 1] & 0xFEu) | ((address >> 10) & 1u); // 2 KB window: PPU A10 replaces bit 0
    }
    return ((bank % (chr_size_bytes >> 10)) << 10) | (address & 0x3FF);
}

static uint16_t rambo1_get_ciram_addr(uint16_t address)
{
    uint16_t a10 = mirror ? ((address >> 11) & 1) : ((address >> 10) & 1); // F-041
    return (uint16_t)((a10 << 10) | (address & 0x3FF));
}

// --- the interrupt counter ------------------------------------------------------------------------------------------
// One counted A12 pulse (F-021, F-037, F-038, F-052, F-055, F-056, F-075)
static void a12_clock(void)
{
    bool expire;
    if (reload) { // the reload that follows $C001; expires if the latch is 0
        counter = latch;
        reload = false;
        expire = (counter == 0);
    } else if (counter == 0) {
        counter = latch; // the automatic reload at zero never expires: it matters only for latch 0 (F-075)
        expire = false;
    } else {
        counter--;
        expire = (counter == 0);
    }
    if (expire) {
        assert_in = 2;
    }
}

// The end of one M2 cycle; a12 is PPU A12 at its falling edge
static void rambo1_cpu_tick(void)
{
    bool a12 = a12_high;
    // A12 filter (F-022, F-051): counts M2 falls with A12 low; a fall with A12 high clocks the counter if the filter is
    // exactly full and resets it either way
    if (a12) {
        if (filt == A12_FILTER_FULL && !cpu_mode) {
            a12_clock();
        }
        filt = 0;
    } else if (filt < A12_FILTER_FULL) {
        filt++;
    }
    // /IRQ asserts one M2 fall after the clocking fall (F-050), and only if enabled at that fall: an $E001 in the cycle is in
    // time, an $E000 in it prevents the assertion (F-074)
    if (assert_in != 0 && --assert_in == 0 && enabled) {
        set_irq(true);
    }
    // CPU-cycle mode (F-018, F-019, F-028, F-031)
    if (timer_on) {
        t++;
        if (t == 5) { // the first interval takes the latch as it is 5 cycles after the write
            next = 4 * ((int32_t)latch + 1) + 2;
            next_valid = true;
        }
        if (next_valid && t == next) {
            if (enabled) { // an expiry while disabled is lost (F-031)
                set_irq(true);
            }
            next = t + 4 * ((int32_t)latch + 1);
        }
    }
}

static void rambo1_ppu_a12(bool high, uint64_t dot)
{
    (void)dot;
    a12_high = high;
}

static void write_c001(uint8_t value)
{
    // Every write restarts the counter in the mode it selects and carries nothing over from the other mode (F-024, F-026,
    // F-057); it never releases /IRQ (F-025)
    cpu_mode = (value & BIT_0) != 0;
    assert_in = 0;
    if (cpu_mode) {
        timer_on = true;
        t = -1; // the cycle() that ends this write cycle makes t = 0
        next_valid = false;
    } else {
        timer_on = false; // a pending CPU-mode expiry is cancelled (F-057)
        reload = true;
        if (filt == A12_FILTER_FULL) {
            filt = A12_FILTER_HELD; // F-052: the next rising edge then does not clock
        }
    }
}

// --- the bus --------------------------------------------------------------------------------------------------------
static uint8_t rambo1_read_prg(uint16_t address)
{
    if (address >= 0x8000) {
        return prg_rom_storage[prg_offset(address)];
    }
    return cpu_get_open_bus();
}

static void rambo1_write_prg(uint16_t address, uint8_t value)
{
    if (address < 0x8000) {
        return;
    }
    bool odd = (address & 1) != 0; // A1-A12 take no part (F-063)
    switch ((address >> 13) & 3) {
    case 0:
        if (!odd) {
            ctrl = value; // bit 4 does nothing
        } else {
            unsigned index = ctrl & 15u;
            if (index <= 5 || index == 8 || index == 9) {
                reg[index] = value; // R8 and R9 take writes whatever K is (F-049)
            } else if (index == 6 || index == 7 || index == 15) {
                reg[index] = value & 0x1F; // F-010
            } // 10-14 store nothing visible (F-039)
        }
        break;
    case 1:
        if (!odd) {
            mirror = value & 1; // F-041; $A001 does nothing visible (F-042, F-053)
        }
        break;
    case 2:
        if (!odd) {
            latch = value;
        } else {
            write_c001(value);
        }
        break;
    default:
        if (!odd) { // $E000: release /IRQ and disable; the counter is not disturbed (F-029)
            enabled = false;
            set_irq(false);
        } else { // $E001: enable only, it does not release /IRQ (F-030)
            enabled = true;
        }
        break;
    }
}

static uint8_t rambo1_read_chr(uint16_t address)
{
    assert(address < 0x2000);
    return chr_storage[chr_offset(address)];
}

static void rambo1_write_chr(uint16_t address, uint8_t value)
{
    assert(address < 0x2000);
    if (chr_is_ram) {
        chr_storage[chr_offset(address)] = value;
    }
}

// --- debugger -------------------------------------------------------------------------------------------------------
static int32_t rambo1_debug_prg_offset(uint16_t address)
{
    return (int32_t)prg_offset(address);
}

static int32_t rambo1_debug_chr_offset(uint16_t address)
{
    return (int32_t)chr_offset(address);
}

static const uint8_t *rambo1_debug_chr_data(size_t *size)
{
    *size = chr_size_bytes;
    return chr_storage;
}

static uint8_t rambo1_debug_read_prg(uint16_t address)
{
    return (address >= 0x8000) ? rambo1_read_prg(address) : (uint8_t)(address >> 8);
}

// --- setup ----------------------------------------------------------------------------------------------------------
static void rambo1_deinit(void)
{
    free(prg_rom_storage);
    prg_rom_storage = NULL;
    prg_rom_size_bytes = 0;
    free(chr_storage);
    chr_storage = NULL;
    chr_size_bytes = 0;
}

static void rambo1_state(state_io *io)
{
    STATE(io, ctrl);
    STATE(io, reg);
    STATE(io, mirror);
    STATE(io, latch);
    STATE(io, enabled);
    STATE(io, irq);
    STATE(io, cpu_mode);
    STATE(io, filt);
    STATE(io, reload);
    STATE(io, counter);
    STATE(io, assert_in);
    STATE(io, timer_on);
    STATE(io, t);
    STATE(io, next_valid);
    STATE(io, next);
    STATE(io, a12_high);
    if (chr_is_ram) {
        state_bytes(io, "rambo1.chr_ram", chr_storage, chr_size_bytes);
    }
    STATE_SKIP(prg_rom_storage); // (the pointers, sizes and the kind of CHR come from the ROM)
    STATE_SKIP(prg_rom_size_bytes);
    STATE_SKIP(chr_storage);
    STATE_SKIP(chr_size_bytes);
    STATE_SKIP(chr_is_ram);
}

bool mapper_rambo1_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info)
{
    assert(ines_file_data != NULL);
    assert(p_mapper_info != NULL);
    const ines_header *h = &ines_file_data->header;
    if (h->prg_16kb_banks < 1) {
        LOG_ERROR(MAPPER, "RAMBO-1 needs PRG-ROM");
        return false;
    }
    prg_rom_size_bytes = 16 * 1024 * h->prg_16kb_banks;
    chr_is_ram = (0 == h->chr_8kb_banks);
    chr_size_bytes = chr_is_ram ? 8 * 1024 : 8 * 1024 * h->chr_8kb_banks;
    prg_rom_storage = malloc(prg_rom_size_bytes);
    chr_storage = calloc(1, chr_size_bytes);
    if ((NULL == prg_rom_storage) || (NULL == chr_storage)) {
        LOG_ERROR(MAPPER, "unable to allocate PRG/CHR storage");
        rambo1_deinit();
        return false;
    }
    memcpy(prg_rom_storage, ines_file_data->prg_rom, prg_rom_size_bytes);
    if (!chr_is_ram) {
        memcpy(chr_storage, ines_file_data->chr_rom, chr_size_bytes);
    }
    chip_power_on(); // (the iNES mirroring bit means nothing to this chip: it powers on vertical, F-035)

    p_mapper_info->deinit = rambo1_deinit;
    p_mapper_info->read_prg = rambo1_read_prg;
    p_mapper_info->write_prg = rambo1_write_prg;
    p_mapper_info->read_chr = rambo1_read_chr;
    p_mapper_info->write_chr = rambo1_write_chr;
    p_mapper_info->get_ciram_addr = rambo1_get_ciram_addr;
    p_mapper_info->debug_read_chr = rambo1_read_chr;
    p_mapper_info->debug_chr_offset = rambo1_debug_chr_offset;
    p_mapper_info->debug_chr_data = rambo1_debug_chr_data;
    p_mapper_info->debug_prg_offset = rambo1_debug_prg_offset;
    p_mapper_info->prg_bank_size = 0x2000;
    p_mapper_info->debug_read_prg = rambo1_debug_read_prg;
    p_mapper_info->state = rambo1_state;
    p_mapper_info->ppu_a12 = rambo1_ppu_a12; // (no a12_rises_to_irq: the PPU catches up only far from the picture)
    p_mapper_info->cpu_tick = rambo1_cpu_tick;
    return true;
}
