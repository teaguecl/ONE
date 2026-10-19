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

/* ExROM / MMC5 (iNES mapper 5): PRG modes 0-3 with ROM or RAM banks, up to 128KB of PRG-RAM (64KB when an iNES 1.0 header says
 * nothing), CHR modes 0-3 with the sprite (A) and background (B) register sets, the name tables ($5105: CIRAM page 0 / 1, ExRAM,
 * fill mode), ExRAM (as name table, extended attributes, RAM), the vertical split, the multiplier, the scanline IRQ, and the
 * expansion audio (two pulse channels and a PCM channel; the levels are being fitted to Mesen's recording of
 * tests/exrom/mmc5audio.nes).
 *
 * The scanline counter works as on the cartridge: the MMC5 watches the PPU bus (mapper_info_t::ppu_fetch). Three reads
 * of the same name table address in a row mean a new scanline (the unused fetches at the end of a line and the first of the
 * next); the first one after the "in frame" flag is clear starts a frame, the others count lines. Three CPU cycles (nine
 * dots) without a PPU read clear the flag. The behavior is Mesen's. PPU catch-up (the CPU running ahead of the PPU) works
 * with it: exrom_lines_to_irq() says how many scanline starts it takes before the IRQ line may rise, and every CPU access to the
 * mapper flushes the PPU first.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "mapper.h"
#include "mapper_exrom.h"
#include "ines.h"
#include "bits.h"
#include "cpu.h"
#include "ppu.h"
#include "rp2a03.h"
#include "savestate.h"

#define EXRAM_SIZE      1024
#define PRG_RAM_DEFAULT (64 * 1024)
#define PRG_RAM_MAX     (128 * 1024)
#define IRQ_BIT         BIT_2
#define IDLE_DOTS       9 // 3 CPU cycles without a PPU read end the frame
#define IRQ_DELAY_DOTS  3
#define IN_FRAME_DELAY_DOTS                                                                                            \
    8 // the flag shows 8 dots after a frame starts (Gemfire, Romance of the Three Kingdoms II; 7 and 9 differ)
static uint8_t *prg_rom_storage = NULL;
static uint32_t prg_rom_size_bytes = 0;
static uint8_t *prg_ram_storage = NULL;
static uint32_t prg_ram_size_bytes = 0; // 0: none (NES 2.0 can say so)
static uint8_t *chr_storage = NULL;     // CHR-ROM, or 8KB of CHR-RAM
static uint32_t chr_size_bytes = 0;
static bool chr_is_ram = false;
static uint8_t exram[EXRAM_SIZE];

// registers
static uint8_t prg_mode;     // $5100
static uint8_t chr_mode;     // $5101
static uint8_t ram_protect1; // $5102
static uint8_t ram_protect2; // $5103
static uint8_t exram_mode;   // $5104
static uint8_t nt_mapping;   // $5105
static uint8_t fill_tile;    // $5106
static uint8_t fill_attr;    // $5107 (2 bits)
static uint8_t prg_reg[5];   // $5113-$5117 ([0]: the RAM at $6000)
static uint16_t chr_reg[12]; // $5120-$512B with the upper bits ($5130) as they were written
static uint8_t chr_upper;    // $5130
static bool chr_last_b;      // the register written last belongs to the B set ($5128-$512B)
static uint8_t split_reg[3]; // $5200-$5202 (the vertical split: not built, 5c)
static uint8_t irq_target;   // $5203
static bool irq_enabled;     // $5204 bit 7
static bool irq_pending;
static bool irq_line_due; // a scanline hit set irq_pending; the IRQ line follows IRQ_DELAY_DOTS later
static uint64_t irq_due_dot;
static uint8_t mult_a, mult_b; // $5205, $5206

// derived from the registers (also in save states, which keeps load simple)
static uint32_t prg_offset[4];    // 8KB slots at $8000, $A000, $C000, $E000: byte offset in ROM or in RAM
static bool prg_slot_ram[4];      // ... which of the two
static uint32_t prg_ram_6000;     // offset of the RAM bank at $6000
static uint32_t chr_offset[2][8]; // 1KB CHR slots of the A and B sets
static uint16_t nt_base[4];       // get_ciram_addr(): 0 / $400 CIRAM, $800 ExRAM, $C00 fill mode, per name table
// Per-tile overrides while the picture is drawn (extended attributes in ExRAM mode 1, the vertical split): the hook works out what the
// next fetch gets, and the PPU's read takes it (get_ciram_addr() / read_nt() for a name table or attribute byte, read_chr())
static uint8_t bg_counter; // after a name table fetch of a tile: 3 (attribute next), 2 (pattern low), 1 (pattern high)
static uint8_t tile_ex_offset; // the split tile's column, for its attribute fetch
static uint16_t ex_tile_pos;   // the tile's position in its name table (ExRAM mode 1: where its byte is)
static uint8_t split_tile;     // screen column (0-33) of the next background name table fetch of the line
static bool split_tile_in;     // the tile of the fetches now is in the split region
static uint8_t split_row_y;    // the split's pixel row of the tile (0-239): its pattern's fine Y
static uint8_t
    split_attr_y; // the row its name table and attribute bytes come from (the same, but for the line's first tile)
static bool nt_override; // the next name table byte is nt_override_value
static uint8_t nt_override_value;
static bool chr_override; // the next pattern byte comes from chr_override_base + (address & $FF8 | fine y)
static uint32_t chr_override_base;
static int chr_override_fine; // the fine Y to use, or -1 for the address's own
static uint8_t chr_set;       // the set the PPU is reading with now (0: A, 1: B)
static uint8_t ppu_ctrl;      // PPUCTRL as last written (bit 5: 8x16 sprites; bit 4: the background's pattern table)

// the scanline detector
static bool in_frame;
static uint64_t in_frame_dot; // the CPU sees the flag from this PPU time on (a frame start shows IRQ_DELAY_DOTS later)
static uint8_t scanline_counter;
static uint8_t same_reads; // consecutive reads of the same name table address, 0-3
static uint16_t last_read_addr;
static uint64_t last_fetch_dot; // PPU time of the last PPU read

// --- expansion audio: two pulse channels like the 2A03's (no sweep, no period muting) and an 8-bit PCM channel ------------------
// The pulses run at half the CPU clock, the length counters and envelopes at 240 Hz of their own (7457 CPU cycles; they do not
// share the 2A03's frame counter). The output goes to the APU's mixer (rp2a03_set_expansion()), summed linearly.
typedef struct {
    bool enabled;    // $5015
    uint8_t reg_vol; // $5000/$5004: duty, halt / envelope loop, constant volume, volume / envelope period
    uint16_t period; // $5002/$5003, $5006/$5007 (the timer reloads with it)
    uint16_t timer;
    uint8_t duty_pos;
    uint8_t length;
    bool env_start;
    uint8_t env_divider;
    uint8_t env_counter; // 15 -> 0
} mmc5_pulse_t;

static mmc5_pulse_t pulse[2];
static uint16_t audio_divider; // CPU cycles to the next 240 Hz step
static bool audio_odd;         // the pulse timers count every other CPU cycle
static uint8_t pcm_output;     // $5011
static bool pcm_read_mode;     // $5010 bit 0
static bool pcm_irq_enabled;   // $5010 bit 7
static bool
    pcm_irq_pending; // (nothing raises it: Mesen takes no PCM IRQ for a $00 write or a $00 read, tests/exrom/mmc5pcm.nes)
static double audio_pushed = -1.0; // the level the APU mixer has (not saved: the next tick pushes it again)

#define AUDIO_STEP_CYCLES 7457
// Levels in the APU mixer's units (0.0 .. 1.0), fitted to Mesen's recording of tests/exrom/mmc5audio.nes
#define PULSE_LEVEL 0.008436
#define PCM_LEVEL   0.002811

static const uint8_t pulse_duty[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0, 0, 0}, {0, 1, 1, 1, 1, 0, 0, 0}, {1, 0, 0, 1, 1, 1, 1, 1}};
static const uint8_t pulse_length_table[32] = {10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
                                               12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};

static unsigned pulse_volume(const mmc5_pulse_t *p)
{
    if ((0 == p->length) || !pulse_duty[p->reg_vol >> 6][p->duty_pos]) {
        return 0;
    }
    return (p->reg_vol & BIT_4) ? (unsigned)(p->reg_vol & 0x0F) : p->env_counter;
}

static void pulse_step_240(mmc5_pulse_t *p)
{
    if (p->env_start) { // envelope
        p->env_start = false;
        p->env_counter = 15;
        p->env_divider = p->reg_vol & 0x0F;
    } else if (0 == p->env_divider) {
        p->env_divider = p->reg_vol & 0x0F;
        if (p->env_counter > 0) {
            p->env_counter--;
        } else if (p->reg_vol & BIT_5) {
            p->env_counter = 15; // loop
        }
    } else {
        p->env_divider--;
    }
    if ((p->length > 0) && !(p->reg_vol & BIT_5)) {
        p->length--;
    }
}

static void exrom_cpu_tick(void)
{
    audio_odd = !audio_odd;
    if (audio_odd) {
        for (int i = 0; i < 2; i++) {
            if (0 == pulse[i].timer) {
                pulse[i].timer = pulse[i].period;
                pulse[i].duty_pos = (uint8_t)((pulse[i].duty_pos + 1) & 7);
            } else {
                pulse[i].timer--;
            }
        }
    }
    if (0 == --audio_divider) {
        audio_divider = AUDIO_STEP_CYCLES;
        pulse_step_240(&pulse[0]);
        pulse_step_240(&pulse[1]);
    }
    double level = (pulse_volume(&pulse[0]) + pulse_volume(&pulse[1])) * PULSE_LEVEL + pcm_output * PCM_LEVEL;
    if (level != audio_pushed) {
        audio_pushed = level;
        rp2a03_set_expansion(level);
    }
}

static void audio_write(uint16_t address, uint8_t value)
{
    mmc5_pulse_t *p = &pulse[(address >> 2) & 1];
    switch (address) {
    case 0x5000:
    case 0x5004: p->reg_vol = value; break;
    case 0x5002:
    case 0x5006: p->period = (uint16_t)((p->period & 0x700) | value); break;
    case 0x5003:
    case 0x5007:
        p->period = (uint16_t)((p->period & 0xFF) | ((value & 7) << 8));
        if (p->enabled) {
            p->length = pulse_length_table[value >> 3];
        }
        p->duty_pos = 0;
        p->env_start = true;
        break;
    case 0x5010:
        pcm_read_mode = (value & BIT_0) != 0;
        pcm_irq_enabled = (value & BIT_7) != 0;
        break;
    case 0x5011:
        if (!pcm_read_mode && (0 != value)) { // (a $00 write does nothing: Mesen's trace of tests/exrom/mmc5pcm.nes)
            pcm_output = value;
        }
        break;
    case 0x5015:
        for (int i = 0; i < 2; i++) {
            pulse[i].enabled = (value >> i) & 1;
            if (!pulse[i].enabled) {
                pulse[i].length = 0;
            }
        }
        break;
    default: break;
    }
}

static void update_prg(void)
{
    uint32_t banks = prg_rom_size_bytes >> 13; // 8KB banks
    uint32_t ram_pages = prg_ram_size_bytes >> 13;
    for (int slot = 0; slot < 4; slot++) {
        uint8_t value;
        uint32_t bank; // in 8KB units
        bool rom = true;
        switch (prg_mode) {
        case 0:
            value = prg_reg[4];
            bank = (value & 0x7C) + slot;
            break;
        case 1:
            if (slot < 2) {
                value = prg_reg[2];
                rom = (value & BIT_7) != 0;
                bank = (value & 0x7E) + slot;
            } else {
                value = prg_reg[4];
                bank = (value & 0x7E) + (slot - 2);
            }
            break;
        case 2:
            if (slot < 2) {
                value = prg_reg[2];
                rom = (value & BIT_7) != 0;
                bank = (value & 0x7E) + slot;
            } else if (slot == 2) {
                value = prg_reg[3];
                rom = (value & BIT_7) != 0;
                bank = value & 0x7F;
            } else {
                bank = prg_reg[4] & 0x7F;
            }
            break;
        default:
            value = prg_reg[slot + 1];
            rom = (slot == 3) || ((value & BIT_7) != 0);
            bank = value & 0x7F;
            break;
        }
        prg_slot_ram[slot] = !rom;
        if (rom) {
            prg_offset[slot] = (bank % banks) << 13;
        } else {
            prg_offset[slot] = (ram_pages > 0) ? (bank % ram_pages) << 13 : 0;
        }
    }
    prg_ram_6000 = (ram_pages > 0) ? ((prg_reg[0] & 0x0F) % ram_pages) << 13 : 0;
}

// CHR: set A ($5120-$5127) and set B ($5128-$512B), each as eight 1KB slots
static void update_chr(void)
{
    uint32_t size = chr_size_bytes;
    for (uint32_t s = 0; s < 8; s++) {
        uint32_t a, b; // byte offsets
        switch (chr_mode) {
        case 0:
            a = ((uint32_t)chr_reg[7] << 13) + (s << 10);
            b = ((uint32_t)chr_reg[11] << 13) + (s << 10);
            break;
        case 1:
            a = ((uint32_t)chr_reg[(s < 4) ? 3 : 7] << 12) + ((s & 3) << 10);
            b = ((uint32_t)chr_reg[11] << 12) + ((s & 3) << 10);
            break;
        case 2:
            a = ((uint32_t)chr_reg[(s | 1)] << 11) + ((s & 1) << 10);
            b = ((uint32_t)chr_reg[(s & 2) ? 11 : 9] << 11) + ((s & 1) << 10);
            break;
        default:
            a = ((uint32_t)chr_reg[s] << 10);
            b = ((uint32_t)chr_reg[8 + (s & 3)] << 10);
            break;
        }
        chr_offset[0][s] = a % size;
        chr_offset[1][s] = b % size;
    }
}

static void update_nt(void)
{
    for (unsigned nt = 0; nt < 4; nt++) {
        nt_base[nt] = (uint16_t)(((nt_mapping >> (nt * 2)) & 3) << 10);
    }
}

static void update_irq_line(void)
{
    irq_line_due = false;
    cpu_irq_request((irq_pending && irq_enabled) || (pcm_irq_pending && pcm_irq_enabled), IRQ_BIT);
}

// The IRQ line of a hit, once its time has come. A fetch runs here at the start of its dot, ahead of the CPU's view of it
// (see A12_DELAY_DOTS in ppu.c), so the line follows IRQ_DELAY_DOTS later; checked at the next fetch (every 2 dots while
// the picture is drawn) and whenever the CPU touches the mapper. 3 (like 4) makes the whole Castlevania III trace
// identical to Mesen's; 2 and 5 do not.
static void irq_line_catch_up(void)
{
    if (irq_line_due && ((int64_t)(ppu_dot_now() - irq_due_dot) >= 0)) {
        update_irq_line();
    }
}

// "In frame" ends when no PPU read came for three CPU cycles, and the run of identical reads starts over. Worked out when
// someone looks, from the PPU time
static bool in_frame_seen(uint64_t now)
{
    return in_frame && ((int64_t)(now - in_frame_dot) >= 0);
}

static void frame_idle_check(uint64_t now)
{
    int64_t idle = (int64_t)(now - last_fetch_dot);
    if ((idle >= IDLE_DOTS) || (idle < 0)) {
        in_frame = false;
        same_reads = 0;
    }
}

static void start_of_scanline(void)
{
    if (!in_frame) {
        in_frame = true;
        in_frame_dot = last_fetch_dot + IN_FRAME_DELAY_DOTS;
        scanline_counter = 0;
        irq_pending = false;
        update_irq_line();
    } else {
        scanline_counter++;
        if (scanline_counter == irq_target) {
            irq_pending = true;
            irq_line_due = true;
            irq_due_dot = last_fetch_dot + IRQ_DELAY_DOTS;
        }
    }
}

// What the PPU gets for this fetch instead of the cartridge's name table or CHR: ExRAM mode 1 gives each tile its own palette and
// 4KB CHR bank (byte of ExRAM at the tile's position: bits 0-5 bank, 6-7 palette), and the vertical split ($5200-$5202)
// shows the ExRAM name table, with its own scroll and CHR bank, in the columns left or right of a boundary (and extended attributes: also for the first two
// tiles of the picture's first line, fetched before the frame starts: Mesen's pictures). The fetches of a tile
// come in the order name table, attribute, pattern low, pattern high
static void tile_overrides(uint16_t address, ppu_fetch_kind kind)
{
    nt_override = false;
    chr_override = false;
    if (kind == PPU_FETCH_CPU) {
        return; // (a $2007 read in between: the picture's fetches go on)
    }
    if ((kind != PPU_FETCH_BG) || (exram_mode > 1)) {
        bg_counter = 0;
        split_tile = 0; // (the sprite fetches and the unused ones: the next tiles are the line's first)
        return;
    }
    bool split_on = (split_reg[0] & BIT_7) != 0;
    if ((address >= 0x2000) && ((address & 0x3FF) < 0x3C0)) { // a tile's name table fetch
        bg_counter = 3;
        unsigned tile = split_tile++;
        unsigned boundary = split_reg[0] & 0x1F;
        split_tile_in = split_on && (tile <= 31) && ((split_reg[0] & BIT_6) ? (tile >= boundary) : (tile < boundary));
        if (split_tile_in) {
            // the split's row of this line: the counter after its start + 1; the first two tiles are fetched in the line before (dots
            // 321-336), tile 0 with that line's counter, tile 1 one row further (Mesen's pictures of tests/exrom/pic_split_scroll_a)
            unsigned y = ((unsigned)split_reg[1] + scanline_counter + 1 + ((1 == tile) ? 1u : 0u)) % 240;
            split_row_y = (uint8_t)y;
            tile_ex_offset = (uint8_t)tile;
            nt_override = true;
            // (the name table and attribute bytes of tile 2, the line's first read, are those of the row before: the read starts the
            // line only after it; its pattern bytes use this line's)
            unsigned y_nt = (2 == tile) ? (y + 239) % 240 : y;
            split_attr_y = (uint8_t)y_nt;
            nt_override_value = exram[((y_nt >> 3) << 5) | tile];
        }
        ex_tile_pos = (uint16_t)(address & 0x3FF);
        return;
    }
    if (bg_counter == 0) {
        return;
    }
    bg_counter--;
    if (bg_counter == 2) { // the attribute byte
        if (split_tile_in) {
            unsigned attr = exram[0x3C0 + ((split_attr_y >> 5) << 3) + (tile_ex_offset >> 2)];
            unsigned palette = (attr >> (((split_attr_y & 0x10) >> 2) | (tile_ex_offset & 2))) & 3;
            nt_override = true;
            nt_override_value = (uint8_t)(palette * 0x55);
        } else if (exram_mode == 1) {
            nt_override = true;
            nt_override_value = (uint8_t)((exram[ex_tile_pos] >> 6) * 0x55);
        }
    } else { // pattern low (1), high (0)
        if (split_tile_in) {
            chr_override = true;
            chr_override_base = ((uint32_t)split_reg[2] | ((uint32_t)chr_upper << 6)) * 0x1000u;
            chr_override_fine = split_row_y & 7;
        } else if (exram_mode == 1) {
            chr_override = true;
            chr_override_base = ((uint32_t)(exram[ex_tile_pos] & 0x3F) | ((uint32_t)chr_upper << 6)) * 0x1000u;
            chr_override_fine = -1;
        }
    }
}

static void exrom_ppu_fetch(uint16_t address, ppu_fetch_kind kind, uint64_t dot)
{
    irq_line_catch_up();
    frame_idle_check(dot);
    last_fetch_dot = dot;
    if (address >= 0x2000) {
        address &= 0x2FFF; // ($3000-$3EFF: the name table mirror)
    }
    // CHR set: 8x8 sprites use A for everything; 8x16: sprites A, background B, the CPU's $2007 the set written last
    if (!(ppu_ctrl & BIT_5)) {
        chr_set = 0;
    } else if (kind == PPU_FETCH_SPRITE) {
        chr_set = 0;
    } else if ((kind == PPU_FETCH_BG) || (kind == PPU_FETCH_IDLE)) {
        chr_set = 1;
    } else {
        chr_set = chr_last_b ? 1 : 0;
    }
    if (address >= 0x2000) {
        same_reads = (address == last_read_addr) ? (uint8_t)(same_reads + 1) : 0;
    } else {
        same_reads = 0;
    }
    last_read_addr = address;
    if (same_reads == 2) {
        start_of_scanline();
        split_tile = 2; // (this read is the line's first fetch, of the third tile)
    }
    tile_overrides(address, kind);
}

// PPU catch-up: how many scanline starts can pass before the IRQ line may rise. A start counts a line when the picture is in
// frame (the counter reaching the compare value $5203 sets the flag), else it starts the frame (counter 0, no compare): then
// the compare value needs that many more. A compare value of 0 never matches (the counter is 0 only at the start of a frame).
// UINT32_MAX: nothing to wait for (IRQ off, or the line is already high: only a CPU access changes it)
static uint32_t exrom_lines_to_irq(void)
{
    if (!irq_enabled || cpu_irq_requested(IRQ_BIT) || (0 == irq_target)) {
        return UINT32_MAX;
    }
    if (irq_line_due) {
        return 0;
    }
    if (in_frame && (scanline_counter < irq_target)) {
        return (uint32_t)(irq_target - scanline_counter);
    }
    return (uint32_t)irq_target + 1u; // (the frame starts again first)
}

static void exrom_ppu_ctrl_write(uint8_t value)
{
    ppu_ctrl = value;
}

static uint8_t exrom_read_chr(uint16_t address)
{
    assert(address < 0x2000);
    if (chr_override) {
        uint32_t fine = (chr_override_fine >= 0) ? (uint32_t)chr_override_fine : (address & 7u);
        return chr_storage[(chr_override_base + ((address & 0xFF8u) | fine)) % chr_size_bytes];
    }
    return chr_storage[chr_offset[chr_set][(address >> 10) & 7] | (address & 0x3FF)];
}

static void exrom_write_chr(uint16_t address, uint8_t value)
{
    assert(address < 0x2000);
    if (chr_is_ram) {
        chr_storage[chr_offset[chr_set][(address >> 10) & 7] | (address & 0x3FF)] = value;
    }
}

// The debugger's views: with 8x16 sprites the pattern table the background uses (PPUCTRL bit 4) shows set B, the other one set A (the
// sprites'); else set A
static uint32_t debug_chr_slot(uint16_t address)
{
    unsigned set = ((ppu_ctrl & BIT_5) && (((address >> 12) & 1) == ((ppu_ctrl >> 4) & 1u))) ? 1u : 0u;
    return chr_offset[set][(address >> 10) & 7] | (address & 0x3FF);
}

static uint8_t exrom_debug_read_chr(uint16_t address)
{
    return chr_storage[debug_chr_slot(address)];
}

static int32_t exrom_debug_chr_offset(uint16_t address)
{
    return (int32_t)debug_chr_slot(address);
}

static const uint8_t *exrom_debug_chr_data(size_t *size)
{
    *size = chr_size_bytes;
    return chr_storage;
}

static uint16_t exrom_get_ciram_addr(uint16_t address)
{
    if (nt_override) {
        return 0xE00;
    }
    return nt_base[(address >> 10) & 3] | (address & 0x3FF);
}

static uint8_t exrom_read_nt(uint16_t address)
{
    if (nt_override) {
        nt_override = false;
        return nt_override_value;
    }
    uint16_t offset = address & 0x3FF;
    if (nt_base[(address >> 10) & 3] == 0x800) {
        return (exram_mode <= 1) ? exram[offset] : 0;
    }
    // fill mode: the tile, and the attribute bits in all four quadrants
    return (offset < 0x3C0) ? fill_tile : (uint8_t)(fill_attr * 0x55);
}

static void exrom_write_nt(uint16_t address, uint8_t value)
{
    if ((nt_base[(address >> 10) & 3] == 0x800) && (exram_mode <= 1)) {
        exram[address & 0x3FF] = value;
    }
}

static bool ram_writable(void)
{
    return ((ram_protect1 & 3) == 2) && ((ram_protect2 & 3) == 1);
}

static uint8_t exrom_read_prg(uint16_t address)
{
    if (address >= 0x8000) {
        unsigned slot = (address >> 13) & 3;
        if (!prg_slot_ram[slot]) {
            uint8_t value = prg_rom_storage[prg_offset[slot] | (address & 0x1FFF)];
            if (pcm_read_mode && (address < 0xC000) &&
                (0 != value)) {     // PCM read mode: the byte the CPU reads at $8000-$BFFF is the
                pcm_output = value; // sample (a $00 does nothing: Mesen, tests/exrom/mmc5pcm.nes)
            }
            return value;
        }
        if (prg_ram_size_bytes > 0) {
            return prg_ram_storage[prg_offset[slot] | (address & 0x1FFF)];
        }
    } else if ((address >= 0x6000) && (prg_ram_size_bytes > 0)) {
        return prg_ram_storage[prg_ram_6000 | (address & 0x1FFF)];
    }
    return cpu_get_open_bus();
}

static uint8_t exrom_debug_read_prg(uint16_t address)
{
    if (address >= 0x8000) {
        unsigned slot = (address >> 13) & 3;
        if (!prg_slot_ram[slot]) {
            return prg_rom_storage[prg_offset[slot] | (address & 0x1FFF)];
        }
        if (prg_ram_size_bytes > 0) {
            return prg_ram_storage[prg_offset[slot] | (address & 0x1FFF)];
        }
    } else if (prg_ram_size_bytes > 0) {
        return prg_ram_storage[prg_ram_6000 | (address & 0x1FFF)];
    }
    return (uint8_t)(address >> 8);
}

static int32_t exrom_debug_prg_offset(uint16_t address)
{
    unsigned slot = (address >> 13) & 3;
    return prg_slot_ram[slot] ? -1 : (int32_t)(prg_offset[slot] | (address & 0x1FFF));
}

static uint8_t exrom_status(void)
{
    return (uint8_t)((irq_pending ? BIT_7 : 0) | (in_frame_seen(ppu_dot_now()) ? BIT_6 : 0));
}

// $4020-$5FFF reads: $5204 (IRQ status; clears the pending flag), the multiplier, ExRAM when it is RAM
static bool exrom_read_exp(uint16_t address, uint8_t *value)
{
    if (address == 0x5204) {
        irq_line_catch_up();
        frame_idle_check(ppu_dot_now());
        *value = exrom_status();
        irq_pending = false;
        update_irq_line();
        return true;
    }
    if (address == 0x5010) { // the PCM IRQ flag
        *value = pcm_irq_pending ? BIT_7 : 0;
        pcm_irq_pending = false;
        update_irq_line();
        return true;
    }
    if (address == 0x5015) { // which pulse channels still have a length
        *value = (uint8_t)((pulse[0].length ? BIT_0 : 0) | (pulse[1].length ? BIT_1 : 0));
        return true;
    }
    if (address == 0x5205) {
        *value = (uint8_t)(mult_a * mult_b);
        return true;
    }
    if (address == 0x5206) {
        *value = (uint8_t)((mult_a * mult_b) >> 8);
        return true;
    }
    if ((address >= 0x5C00) && (address <= 0x5FFF) && (exram_mode >= 2)) {
        *value = exram[address & 0x3FF];
        return true;
    }
    return false;
}

// (Mesen's debugger shows the IRQ status and the multiplier as unmapped memory, the address high byte; ExRAM when it is RAM)
static bool exrom_debug_read_exp(uint16_t address, uint8_t *value)
{
    if ((address >= 0x5C00) && (address <= 0x5FFF) && (exram_mode >= 2)) {
        return exrom_read_exp(address, value);
    }
    return false;
}

static void exrom_write_prg(uint16_t address, uint8_t value)
{
    if (address >= 0x8000) {
        unsigned slot = (address >> 13) & 3;
        if (prg_slot_ram[slot] && (prg_ram_size_bytes > 0) && ram_writable()) {
            prg_ram_storage[prg_offset[slot] | (address & 0x1FFF)] = value;
        }
    } else if (address >= 0x6000) {
        if ((prg_ram_size_bytes > 0) && ram_writable()) {
            prg_ram_storage[prg_ram_6000 | (address & 0x1FFF)] = value;
        }
    } else if (address >= 0x5C00) { // ExRAM
        if (exram_mode <= 1) {      // as nametable: the CPU can only write while the picture is drawn (else it clears)
            frame_idle_check(ppu_dot_now());
            exram[address & 0x3FF] = in_frame_seen(ppu_dot_now()) ? value : 0;
        } else if (exram_mode == 2) {
            exram[address & 0x3FF] = value;
        }
    } else if (address >= 0x5000) {
        switch (address) {
        case 0x5100:
            prg_mode = value & 3;
            update_prg();
            break;
        case 0x5101:
            chr_mode = value & 3;
            update_chr();
            break;
        case 0x5102: ram_protect1 = value; break;
        case 0x5103: ram_protect2 = value; break;
        case 0x5104: exram_mode = value & 3; break;
        case 0x5105:
            nt_mapping = value;
            update_nt();
            break;
        case 0x5106: fill_tile = value; break;
        case 0x5107: fill_attr = value & 3; break;
        case 0x5113:
        case 0x5114:
        case 0x5115:
        case 0x5116:
        case 0x5117:
            prg_reg[address - 0x5113] = value;
            update_prg();
            break;
        case 0x5130: chr_upper = value & 3; break;
        case 0x5200:
        case 0x5201:
        case 0x5202: split_reg[address - 0x5200] = value; break;
        case 0x5203: irq_target = value; break;
        case 0x5204:
            irq_enabled = (value & BIT_7) != 0;
            update_irq_line();
            break;
        case 0x5205: mult_a = value; break;
        case 0x5206: mult_b = value; break;
        default:
            if ((address >= 0x5120) && (address <= 0x512B)) {
                chr_reg[address - 0x5120] = (uint16_t)(value | (chr_upper << 8));
                chr_last_b = (address >= 0x5128);
                update_chr();
            } else if (address <= 0x5015) {
                audio_write(address, value);
                if (address == 0x5010) {
                    update_irq_line();
                }
            }
            break;
        }
    }
}

// Debugger edit of the RAM mapped at $6000-$DFFF now (the protection does not matter)
static bool exrom_debug_write_prg(uint16_t address, uint8_t value)
{
    if (0 == prg_ram_size_bytes) {
        return false;
    }
    if (address < 0x8000) {
        prg_ram_storage[prg_ram_6000 | (address & 0x1FFF)] = value;
        return true;
    }
    unsigned slot = (address >> 13) & 3;
    if (address < 0xE000 && prg_slot_ram[slot]) {
        prg_ram_storage[prg_offset[slot] | (address & 0x1FFF)] = value;
        return true;
    }
    return false;
}

// The background view with extended attributes (ExRAM mode 1): the tile's own 4 KB bank and palette
static bool exrom_debug_bg_tile(uint16_t nt_address, int32_t *chr_offset, int *palette)
{
    if (exram_mode != 1) {
        return false;
    }
    uint8_t byte = exram[nt_address & 0x3FF];
    *chr_offset = (int32_t)((((uint32_t)(byte & 0x3F) | ((uint32_t)chr_upper << 6)) * 0x1000u) % chr_size_bytes);
    *palette = byte >> 6;
    return true;
}

static uint8_t *exrom_debug_prg_ram(size_t *size)
{
    *size = prg_ram_size_bytes;
    return prg_ram_storage;
}

static void exrom_get_sav_data(sav_data_t *sav)
{
    sav->data = prg_ram_storage;
    sav->size = prg_ram_size_bytes;
}

static void exrom_deinit(void)
{
    free(prg_rom_storage);
    prg_rom_storage = NULL;
    prg_rom_size_bytes = 0;
    free(prg_ram_storage);
    prg_ram_storage = NULL;
    prg_ram_size_bytes = 0;
    free(chr_storage);
    chr_storage = NULL;
    chr_size_bytes = 0;
    cpu_irq_request(false, IRQ_BIT);
    rp2a03_set_expansion(0.0);
}

static void exrom_state(state_io *io)
{
    audio_pushed = -1.0; // (a loaded state: the mixer gets the level again at the next tick)
    STATE(io, pulse);
    STATE(io, audio_divider);
    STATE(io, audio_odd);
    STATE(io, pcm_output);
    STATE(io, pcm_read_mode);
    STATE(io, pcm_irq_enabled);
    STATE(io, pcm_irq_pending);
    STATE(io, exram);
    STATE(io, prg_mode);
    STATE(io, chr_mode);
    STATE(io, ram_protect1);
    STATE(io, ram_protect2);
    STATE(io, exram_mode);
    STATE(io, nt_mapping);
    STATE(io, fill_tile);
    STATE(io, fill_attr);
    STATE(io, prg_reg);
    STATE(io, chr_reg);
    STATE(io, chr_upper);
    STATE(io, chr_last_b);
    STATE(io, split_reg);
    STATE(io, irq_target);
    STATE(io, irq_enabled);
    STATE(io, irq_pending);
    STATE(io, irq_line_due);
    STATE(io, irq_due_dot);
    STATE(io, mult_a);
    STATE(io, mult_b);
    STATE(io, prg_offset);
    STATE(io, prg_slot_ram);
    STATE(io, prg_ram_6000);
    STATE(io, chr_offset);
    STATE(io, nt_base);
    STATE(io, bg_counter);
    STATE(io, tile_ex_offset);
    STATE(io, ex_tile_pos);
    STATE(io, split_tile);
    STATE(io, split_tile_in);
    STATE(io, split_row_y);
    STATE(io, split_attr_y);
    STATE(io, nt_override);
    STATE(io, nt_override_value);
    STATE(io, chr_override);
    STATE(io, chr_override_base);
    STATE(io, chr_override_fine);
    STATE(io, chr_set);
    STATE(io, ppu_ctrl);
    STATE(io, in_frame);
    STATE(io, in_frame_dot);
    STATE(io, scanline_counter);
    STATE(io, same_reads);
    STATE(io, last_read_addr);
    STATE(io, last_fetch_dot);
    if (prg_ram_storage != NULL) {
        state_bytes(io, "exrom.prg_ram", prg_ram_storage, prg_ram_size_bytes);
    }
    if (chr_is_ram) {
        state_bytes(io, "exrom.chr_ram", chr_storage, chr_size_bytes);
    }
    STATE_SKIP(audio_pushed);
    STATE_SKIP(prg_rom_storage); // (the pointers and sizes come from the ROM)
    STATE_SKIP(prg_rom_size_bytes);
    STATE_SKIP(prg_ram_storage);
    STATE_SKIP(prg_ram_size_bytes);
    STATE_SKIP(chr_storage);
    STATE_SKIP(chr_size_bytes);
    STATE_SKIP(chr_is_ram);
}

// PRG-RAM size: NES 2.0 says (battery-backed first); an iNES 1.0 header does not, so 64KB
static bool exrom_ram_size(const ines_header *h, uint32_t *chr_ram_size)
{
    *chr_ram_size = 8 * 1024;
    prg_ram_size_bytes = PRG_RAM_DEFAULT;
    if (mapper_is_nes2(h)) {
        nes2_info_t nes2;
        if (!mapper_nes2_info(h, &nes2)) {
            return false;
        }
        prg_ram_size_bytes = (0 != nes2.prg_nvram_size) ? nes2.prg_nvram_size : nes2.prg_ram_size;
        if (prg_ram_size_bytes > PRG_RAM_MAX) {
            LOG_WARN(MAPPER, "NES 2.0 PRG-RAM of %u bytes: the MMC5 maps %u", prg_ram_size_bytes, PRG_RAM_MAX);
            prg_ram_size_bytes = PRG_RAM_MAX;
        }
        prg_ram_size_bytes &= ~0x1FFFu; // (8KB banks)
        if (nes2.chr_ram_size >= 1024) {
            *chr_ram_size = nes2.chr_ram_size;
        }
    }
    return true;
}

bool mapper_exrom_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data)
{
    assert(ines_file_data != NULL);
    assert(p_mapper_info != NULL);
    assert(sav_data != NULL);

    uint32_t chr_ram_size;
    if (!exrom_ram_size(&ines_file_data->header, &chr_ram_size)) {
        return false;
    }
    prg_rom_size_bytes = 16 * 1024 * ines_file_data->header.prg_16kb_banks;
    if ((0 == ines_file_data->header.prg_16kb_banks) || (ines_file_data->header.prg_16kb_banks > 64)) {
        LOG_ERROR(MAPPER, "unsupported PRG size");
        prg_rom_size_bytes = 0;
        prg_ram_size_bytes = 0;
        return false;
    }
    prg_rom_storage = calloc(1, prg_rom_size_bytes);
    if (0 != prg_ram_size_bytes) {
        prg_ram_storage = calloc(1, prg_ram_size_bytes);
    }
    chr_size_bytes = ines_file_data->header.chr_8kb_banks * 8192;
    chr_is_ram = (0 == chr_size_bytes);
    if (chr_is_ram) {
        chr_size_bytes = chr_ram_size;
    }
    chr_storage = calloc(1, chr_size_bytes);
    if ((NULL == prg_rom_storage) || (NULL == chr_storage) ||
        ((0 != prg_ram_size_bytes) && (NULL == prg_ram_storage))) {
        LOG_ERROR(MAPPER, "unable to allocate storage");
        exrom_deinit();
        return false;
    }
    memcpy(prg_rom_storage, ines_file_data->prg_rom, prg_rom_size_bytes);
    if (!chr_is_ram) {
        memcpy(chr_storage, ines_file_data->chr_rom, chr_size_bytes);
    }
    if ((ines_file_data->header.flags_6 & BIT_1) && (sav_data->size > 0) && (NULL != prg_ram_storage)) {
        memcpy(prg_ram_storage, sav_data->data,
               (sav_data->size < prg_ram_size_bytes) ? sav_data->size : prg_ram_size_bytes);
    }

    // power-on: the last PRG bank at $E000 (mode 3), all name tables CIRAM page 0 until $5105 is written (Mesen: not the header's mirroring)
    memset(exram, 0, sizeof(exram));
    memset(chr_reg, 0, sizeof(chr_reg));
    memset(split_reg, 0, sizeof(split_reg));
    prg_mode = 3;
    chr_mode = 0;
    ram_protect1 = ram_protect2 = 0;
    exram_mode = 0;
    nt_mapping = 0x00;
    fill_tile = fill_attr = 0;
    memset(prg_reg, 0,
           sizeof(prg_reg)); // (RAM page 0 at $6000-$DFFF: Mesen's power-on state: tests/exrom/mmc5pcm.nes read mode)
    prg_reg[4] = 0xFF;       // the last ROM bank at $E000
    chr_upper = 0;
    chr_last_b = false;
    irq_target = 0;
    irq_enabled = irq_pending = irq_line_due = false;
    memset(pulse, 0, sizeof(pulse));
    audio_divider = AUDIO_STEP_CYCLES;
    audio_odd = false;
    pcm_output = 0;
    pcm_read_mode = pcm_irq_enabled = pcm_irq_pending = false;
    audio_pushed = -1.0;
    irq_due_dot = 0;
    mult_a = mult_b = 0;
    chr_set = 0;
    ppu_ctrl = 0;
    bg_counter = tile_ex_offset = split_tile = split_row_y = split_attr_y = 0;
    ex_tile_pos = 0;
    split_tile_in = nt_override = chr_override = false;
    nt_override_value = 0;
    chr_override_base = 0;
    chr_override_fine = -1;
    in_frame = false;
    in_frame_dot = 0;
    scanline_counter = same_reads = 0;
    last_read_addr = 0;
    last_fetch_dot = 0;
    update_prg();
    update_chr();
    update_nt();

    p_mapper_info->deinit = exrom_deinit;
    p_mapper_info->read_prg = exrom_read_prg;
    p_mapper_info->write_prg = exrom_write_prg;
    p_mapper_info->read_chr = exrom_read_chr;
    p_mapper_info->write_chr = exrom_write_chr;
    p_mapper_info->get_ciram_addr = exrom_get_ciram_addr;
    p_mapper_info->read_nt = exrom_read_nt;
    p_mapper_info->write_nt = exrom_write_nt;
    p_mapper_info->read_exp = exrom_read_exp;
    p_mapper_info->debug_read_exp = exrom_debug_read_exp;
    p_mapper_info->ppu_fetch = exrom_ppu_fetch;
    p_mapper_info->ppu_ctrl_write = exrom_ppu_ctrl_write;
    p_mapper_info->debug_read_chr = exrom_debug_read_chr;
    p_mapper_info->debug_chr_offset = exrom_debug_chr_offset;
    p_mapper_info->debug_chr_data = exrom_debug_chr_data;
    p_mapper_info->debug_prg_offset = exrom_debug_prg_offset;
    p_mapper_info->prg_bank_size = 0x2000;
    p_mapper_info->debug_prg_ram = exrom_debug_prg_ram;
    p_mapper_info->debug_read_prg = exrom_debug_read_prg;
    p_mapper_info->debug_write_prg = exrom_debug_write_prg;
    p_mapper_info->debug_bg_tile = exrom_debug_bg_tile;
    p_mapper_info->get_sav_data = exrom_get_sav_data;
    p_mapper_info->lines_to_irq = exrom_lines_to_irq;
    p_mapper_info->cpu_tick = exrom_cpu_tick;
    p_mapper_info->state = exrom_state;
    return true;
}
