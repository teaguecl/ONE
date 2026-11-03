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
 * F-090, as of the research repo's commit 544ca1b; the finding numbers in the comments are his), not from another emulator.
 *
 * The chip is modeled as it is: a state machine that sees CPU writes and, once per M2 cycle, the level of PPU A12 at the M2
 * falling edge. In each CPU cycle the writes come first, then the tick (the cpu_tick hook, called at the end of the cycle).
 * It is run lazily, from event to event (A12 edges with their dot times, CPU writes, the ticks at which /IRQ asserts), so that
 * the PPU may run behind the CPU (PPU catch-up: a12_rises_to_irq()); the result is that of running it every cycle.
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
#include "ppu.h"
#include "savestate.h"

#define A12_FILTER_FULL 16 // M2 falls with A12 low before a rise counts (F-022, F-051)
#define A12_FILTER_HELD 17 // "above full": set by a $C001 write while full, the next rise does not clock (F-052)

static uint8_t *prg_rom_storage = NULL;
static uint32_t prg_rom_size_bytes = 0;
static uint8_t *chr_storage = NULL;
static uint32_t chr_size_bytes = 0;
static bool chr_is_ram = false;

// --- the chip -------------------------------------------------------------------------------------------------------
static uint8_t ctrl;    // the last $8000 value: K, P, C and the index take effect at once (F-040)
static uint8_t reg[16]; // R0-R5, R8, R9: 8 bits; R6, R7, RF: 5 bits; the others are never used
static uint8_t mirror;  // 0 vertical, 1 horizontal
static uint8_t latch;   // $C000, shared by both counter modes
static bool enabled;    // $E001 sets, $E000 clears
static bool irq;        // /IRQ asserted; sticky until $E000
static bool cpu_mode;   // $C001 bit 0
static uint8_t filt;    // A12 filter: M2 falls with A12 low, 0-16; 17 after a $C001 write while full
static bool reload;     // A12 mode: the next counted pulse reloads the counter
static uint8_t counter; // A12 mode counter
static bool timer_on;   // CPU mode: the interval timer runs
static bool next_valid; // CPU mode: next is set (5 cycles after the $C001 write, 1 if early)
static bool
    early; // CPU mode: the first expiry is 4 cycles early (F-087: an A12-mode to CPU-mode $C001 with the A12 filter not full)
static bool a12_high; // PPU A12 as the PPU last reported it

// --- time -------------------------------------------------------------------------------------------------------------
// The chip is event-driven, so that the PPU can run behind the CPU (PPU catch-up). Time is counted in M2 cycles, as the
// number of cpu_tick() calls: "tick k" is the end of the k-th CPU cycle, which is where the chip model samples A12 (an M2
// fall). A CPU write in cycle k comes before tick k. The PPU reports A12 edges with the time of the edge in dots, which
// tick_of() turns into the tick that sees it first (one tick per three dots). Between the events (an A12 edge, a CPU write,
// a tick at which /IRQ asserts) the chip is not run: a12_run() and timer_run() bring it to a tick in one step.
#define NEVER    INT64_MAX
#define DOT_BASE 0 // the PPU dot (without the skipped ones) that starts tick 0

static int64_t ticks;    // cpu_tick() calls so far: the tick the next one is
static int64_t wake;     // the tick from which cpu_tick() has to do something (NEVER: nothing is due)
static int64_t a12_done; // A12 side: the filter and counter are right for the ticks before this one
static int64_t
    a12_pend; // a rise of A12 that may count was seen for this tick, which has not been run yet (NEVER: none)
static int64_t assert_tick; // A12 mode: the tick at which /IRQ asserts, one after the clocking one (NEVER: none: F-050)
static int64_t tw;          // CPU mode: the tick of the cycle that wrote $C001; t = tick - tw (F-018)
static int64_t next;        // CPU mode: t of the next expiry

static int64_t tick_of(uint64_t dot)
{
    return ((int64_t)(dot - ppu_dots_skipped()) - DOT_BASE) / 3;
}

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
    cpu_mode = true; // F-088: the chip powers up in CPU-cycle mode, latch $FF, the counter already running:
    timer_on = true; // with no $C000 or $C001 write the first expiry is at M2 fall 1018 (t is 9 at the first tick)
    tw = -9;
    next_valid = true;
    next = 4 * (0xFF + 1) + 2;
    early = false;
    filt =
        A12_FILTER_FULL; // (not full on the chip at power-on: about 8 after a short power cycle, F-085; full keeps the tests, as in the reference model)
    reload = true;
    counter = 0;
    a12_high = false;
    ticks = 0;
    a12_done = 0;
    a12_pend = NEVER;
    assert_tick = NEVER;
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
// One counted A12 pulse, at tick j (F-021, F-037, F-038, F-052, F-055, F-056, F-075)
static void a12_clock(int64_t j)
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
        assert_tick = j + 1;
    }
}

// The A12 filter and counter for the ticks up to UPTO, with A12 at the level it has now (F-022, F-051): the filter counts M2
// falls with A12 low; a fall with A12 high clocks the counter if the filter is exactly full and resets it either way
static void a12_run(int64_t upto)
{
    int64_t n = upto - a12_done + 1;
    if (n <= 0) {
        return;
    }
    if (a12_high) {
        if (filt == A12_FILTER_FULL && !cpu_mode) {
            a12_clock(a12_done);
        }
        filt = 0;
    } else if (filt < A12_FILTER_FULL) {
        filt = (n >= A12_FILTER_FULL - filt) ? A12_FILTER_FULL : (uint8_t)(filt + n);
    }
    a12_done = upto + 1;
    if (a12_pend <= upto) {
        a12_pend = NEVER;
    }
}

// CPU-cycle mode up to tick UPTO (F-018, F-019, F-028, F-031): the first interval takes the latch as it is 5 cycles after the
// write (F-087: 1 cycle after it, and 4 cycles shorter); every expiry sets /IRQ if enabled, and one while disabled is lost (F-031)
static void timer_run(int64_t upto)
{
    if (!timer_on) {
        return;
    }
    if (!next_valid) {
        if (tw + (early ? 1 : 5) > upto) {
            return;
        }
        next = 4 * ((int64_t)latch + 1) + 2 - (early ? 4 : 0);
        next_valid = true;
    }
    if (tw + next <= upto) {
        const int64_t interval = 4 * ((int64_t)latch + 1);
        const int64_t count = (upto - tw - next) / interval + 1;
        if (enabled) {
            set_irq(true);
        }
        next += count * interval;
    }
}

// The tick from which cpu_tick() has to do something
static void update_wake(void)
{
    int64_t w = assert_tick;
    if (a12_pend < w) {
        w = a12_pend;
    }
    if (timer_on && enabled && !irq) {
        const int64_t due = tw + (next_valid ? next : (early ? 1 : 5));
        if (due < w) {
            w = due;
        }
    }
    wake = w;
}

// /IRQ asserts one M2 fall after the clocking fall (F-050), and only if enabled at that fall: an $E001 in the cycle is in
// time, an $E000 in it prevents the assertion (F-074)
static void assert_run(int64_t upto)
{
    if (assert_tick <= upto) {
        assert_tick = NEVER;
        if (enabled) {
            set_irq(true);
        }
    }
}

// The end of one M2 cycle (the cpu_tick hook)
static void rambo1_cpu_tick(void)
{
    const int64_t k = ticks++;
    if (k >= wake) {
        if (a12_pend <= k) {
            a12_run(k); // (a rise is waiting: the PPU is where the CPU is when the level is read)
        }
        assert_run(k);
        timer_run(k);
        update_wake();
    }
}

// Before a CPU write in the cycle that ends with tick TICKS: the chip as it is after the tick before
static void catch_up(void)
{
    assert((int64_t)(ppu_dot_now() - ppu_dots_skipped()) - DOT_BASE ==
           3 * ticks); // (the PPU is at the start of this cycle)
    a12_run(ticks - 1);
    assert_run(ticks - 1);
    timer_run(ticks - 1);
}

static void rambo1_ppu_a12(bool high, uint64_t dot)
{
    int64_t e = tick_of(dot); // the first tick that sees the new level
    if (e < a12_done) {
        e = a12_done; // (an edge for a tick that has run: does not happen)
    }
    a12_run(e - 1);
    if (high && !a12_high && e < a12_pend) {
        a12_pend = e;
    }
    a12_high = high;
    update_wake();
}

static void write_c001(uint8_t value)
{
    // Every write restarts the counter in the mode it selects and carries nothing over from the other mode (F-024, F-026,
    // F-057); it never releases /IRQ (F-025)
    const bool early_switch = (value & BIT_0) != 0 && !cpu_mode && filt < A12_FILTER_FULL; // F-087
    cpu_mode = (value & BIT_0) != 0;
    assert_tick = NEVER;
    if (cpu_mode) {
        early = early_switch; // the first CPU-mode expiry then comes 4 cycles early
        timer_on = true;
        tw = ticks; // the tick that ends this write cycle is t = 0
        next_valid = false;
    } else {
        timer_on = false; // a pending CPU-mode expiry is cancelled (F-057)
        reload = true;
        if (filt == A12_FILTER_FULL) {
            filt = A12_FILTER_HELD; // F-052: the next rising edge then does not clock
        }
    }
}

// PPU catch-up: after how many counted A12 rises can /IRQ be asserted? Only in A12 mode, with /IRQ enabled and not yet asserted
// (an $E001, $E000 or $C001 is a CPU write, which brings the PPU up to date and asks again). The counter is as of the last
// run, so a rise that has not been run (a12_pend) or a clock that is waiting for its assertion is "now".
static uint32_t rambo1_a12_rises_to_irq(void)
{
    if (cpu_mode || !enabled || irq) {
        return UINT32_MAX;
    }
    if (a12_pend != NEVER || assert_tick != NEVER) {
        return 1;
    }
    if (reload) {
        return (latch == 0) ? 1u : 1u + latch;
    }
    if (counter == 0) {
        return (latch == 0) ? UINT32_MAX : 1u + latch; // (the reload at zero does not expire: F-075)
    }
    return counter;
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
    catch_up();
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
    update_wake();
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
    if (!state_loading(io)) {
        catch_up(); // (the chip runs lazily: a state has it at the tick before, whenever the events came)
        update_wake();
    }
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
    STATE(io, timer_on);
    STATE(io, next_valid);
    STATE(io, next);
    STATE(io, early);
    STATE(io, a12_high);
    STATE(io, ticks);
    STATE(io, wake);
    STATE(io, a12_done);
    STATE(io, a12_pend);
    STATE(io, assert_tick);
    STATE(io, tw);
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
    p_mapper_info->ppu_a12 = rambo1_ppu_a12;
    p_mapper_info->a12_rises_to_irq = rambo1_a12_rises_to_irq;
    p_mapper_info->cpu_tick = rambo1_cpu_tick;
    return true;
}
