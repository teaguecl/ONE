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
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "libco/libco.h"
#include "ppu.h"
#include "cpu.h"
#include "mapper.h"
#include "bits.h"
#include "rp2a03.h"
#include "video.h"
#include "savestate.h"
#include "script.h"
#include "log.h"

//#define NINTENDULATOR_075  // define this to indicate trace log compatibility with the 075b build of Nintendulator
#define TRACE_TYPE_MESEN

#if defined(NINTENDULATOR_075) || defined(TRACE_TYPE_MESEN)
#define SCANLINE_START    (0)
#define SKIP_FIRST_VBLANK (false)
#else
#define SCANLINE_START    (241) // set this way so Nintendulator 070 logs line up
// Nintendulator doesn't set the VBL flag on this first scanline.  I think it's an attempt
// to simulate the PPU warm-up: https://wiki.nesdev.com/w/index.php/PPU_power_up_state
#define SKIP_FIRST_VBLANK (true)
#endif

#define PPU_ONE_CYCLE           (4)       /* one PPU cycle is 4 cycles of the reference clock */
#define PPU_TWO_CYCLES          (8)       /* two PPU cycles is 8 cycles of the reference clock */
#define PPU_CLOCKS_PER_SCANLINE (341)     /* 341 PPU cycles in one scanline */
#define REF_CLOCKS_PER_SCANLINE (341 * 4) /* 1364 reference ticks per scanline */

extern cothread_t cpu_thread;
cothread_t ppu_thread;
static int32_t ppu_clock;
static int32_t ppu_clock_requested;
// Fast dots (ppu_add_clocks()): while this is above 0, the next dot has nothing to process but its own bookkeeping:
// no event at its (scanline, dot), no queued A12 change, no pending $2007 work, no trace catch-up, no switch to the
// CPU. Whoever makes such work pending sets it to 0 (the next dot takes the slow path, which works it out again).
static int32_t ppu_fast_dots = 0;
static bool apu_log_active;  // ppu_set_apu_log()
static uint8_t ciram[0x800]; // 2kb of internal ram on PPU

uint8_t ppu_ciram_read(unsigned offset)
{
    return ciram[offset & 0x7FF];
}

void ppu_ciram_write(unsigned offset, uint8_t value)
{
    ciram[offset & 0x7FF] = value;
}
static uint32_t g_frame_num = 0;
// where the last sprite 0 hit happened (debuggers: ppu_debug_sprite0_hit())
static int s0_hit_line = -1, s0_hit_x = -1;
static ppu_line_scroll line_scroll[240]; // (ppu_debug_line_scroll(); recorded in the debugger build only)
static uint32_t s0_hit_frame = 0;

// Init palette with values from blargg's test rom blargg_ppu_tests_2005.09.15b/power_up_palette.nes
// todo: do this at power up rather than here?
static uint8_t bg_palette[0x20] = {0x09, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x0D, 0x08, 0x10, 0x08,
                                   0x24, 0x00, 0x00, 0x04, 0x2C, 0x09, 0x01, 0x34, 0x03, 0x00, 0x04,
                                   0x00, 0x14, 0x08, 0x3A, 0x00, 0x02, 0x00, 0x20, 0x2C, 0x08};
static uint32_t scanline;
//static uint32_t sl_end_tick = 341;  // scanline end tick (num ppu cycles)
static bool short_frame = false; // http://wiki.nesdev.com/w/index.php/PPU_frame_timing
// Odd-frame dot skip decision, latched when the pre-render line reaches dot 338 (before the CPU acts on that dot):
// a $2001 write landing at dot 337 still affects the skip, one landing at 338 or later does not
// (Mesen, ppu_vbl_nmi/10-even_odd_timing: enable@337 skips, enable@338 doesn't; disable@337 prevents, @338 doesn't).
static bool skip_dot_this_frame = false;
static uint32_t
    dots_skipped; // odd-frame dots skipped since power-on (the nominal dot time of ppu_dot_now() counts them)
static uint8_t nmi_occurred; // http://wiki.nesdev.com/w/index.php/NMI
static uint8_t ppudata_buffer;
static uint32_t sl_clock;
static uint8_t io_latch; // hardware used for IO.  Need to track it because it gets returned on reads of $2002 bits 0-4
// PPU open-bus decay (modelled as Mesen does): each latch bit remembers the frame it was last driven; on any register
// access, bits the access doesn't drive are cleared once the frame counter is more than 3 past their stamp
// (measured against Mesen traces of ppu_vbl_nmi/01: a write at line 122 decays at the 4th line-240 tick after it).
// open_bus_frame ticks at dot 0 of scanline 240.
static uint32_t open_bus_frame;
// Trace only: Mesen's debugger peeks $2007 through the PPU's last bus access -- the address of the most recent
// $2006 (second write) / $2007 access, and for palette addresses the value that was there at that moment (before a
// write). Recorded here so the Mesen-format trace can show the same thing (cpu_timing_test6).
// Some PPU dots after an access, the bus catches up with the (incremented) VRAM address: the peek then shows the
// palette byte at the current address. From cpu_timing_test6 the catch-up takes more than 3 and at most 18 dots;
// 6 (one CPU cycle after the access cycle) is a guess within that range.
#define TRACE_BUS_CATCHUP_DOTS 6
static uint16_t trace_bus_addr;
static uint8_t trace_bus_palette;
static int trace_bus_catchup; // dots until the bus follows vram_address; 0 = idle
// Likewise the read buffer: right after a $2007 read Mesen's peek still shows the value that read returned; the
// refilled buffer appears at the catch-up (cpu_dummy_writes_ppumem: LDA $2007 then LDY $2007 peeks $D6, not $CB).
static bool trace_buffer_stale;
static uint8_t trace_buffer_value;
static uint8_t ppu_vram_read(uint16_t address);
static bool ppu_is_rendering();
static inline void ppu_bus_a12_delayed(uint16_t addr, unsigned delay);
static void trace_note_bus_addr(uint16_t addr)
{
    trace_bus_addr = addr & 0x3FFF;
    if (!ppu_is_rendering()) { // while rendering, the fetches drive the bus, not v
        ppu_bus_a12_delayed(trace_bus_addr, 0);
    }
    if (trace_bus_addr >= 0x3F00) {
        trace_bus_palette = ppu_vram_read(trace_bus_addr);
    }
}
static void trace_note_bus_access(uint16_t addr)
{
    trace_note_bus_addr(addr);
    trace_bus_catchup = TRACE_BUS_CATCHUP_DOTS;
    ppu_fast_dots = 0;
    trace_buffer_stale = false;
}
static uint32_t open_bus_stamp[8];
static bool glb_skip_vblank = SKIP_FIRST_VBLANK;
// Dot-ordering during rendering: "do dot N's work, THEN let the CPU observe dot N" (same model as the VBL set
// in ppu_add_clocks()).  The render loop computes pixel x only after ppu_add_clocks() has already advanced to
// dot x+1, so there ppu_add_clocks() runs in deferred mode: it advances the dot but postpones the CPU's turn
// (cpu_switch_owed) until the start of the next ppu_add_clocks() call, i.e. after the pixel work is done.
static bool defer_cpu_switch = false;
static bool cpu_switch_owed = false;
//static bool  glb_skip_nmi = false;
static bool ppu_reset_flag = true; // See https://www.nesdev.org/wiki/PPU_power_up_state

// the rendering latches and shift registers
static uint16_t pattern_bit0_shift_reg;
static uint16_t pattern_bit1_shift_reg;
static uint16_t attribute_bit0_shift_reg;
static uint16_t attribute_bit1_shift_reg;

uint8_t PPUCTRL = 0;
uint8_t PPUMASK = 0;
uint8_t PPUSTATUS = 0; // $2002

uint16_t PPUADDR = 0;
uint16_t PPUSCROLL = 0; // $2005
uint8_t ppu_fine_x_scroll;
bool vram_latch_write = false; // false means "first write", true means "second write"
uint16_t vram_latch = 0;       // AKA "Loopy_T"
uint16_t vram_address = 0;     // AKA "Loopy_V"
uint16_t vram_increment = 1;

uint8_t OAMADDR;
uint8_t obj_attrib_mem[0x100]; // OAM http://wiki.nesdev.com/w/index.php/PPU_OAM
uint8_t
    secondary_oam[32]; // Holds sprites on current scanline, http://wiki.nesdev.com/w/index.php/PPU_sprite_evaluation

/* These are used for sprite rendering: http://wiki.nesdev.com/w/index.php/PPU_rendering */
// Slots 0-7 are the hardware's. Slots 8-63 only exist with "Disable Sprite Limit" (ppu_set_sprite_limit()): the other
// sprites of the line, in OAM order after the first eight, so the priority between sprites stays right. They are not
// saved in save states (a state loaded in mid-line draws the rest of that line without them).
#define SPRITE_SLOTS 64
#define SPRITE_HW    8
static bool sprite_no_limit;                    // video.disable_sprite_limit (a setting, not machine state)
static uint8_t sprite_tiles_bit0[SPRITE_SLOTS]; // tile bitmap shift registers
static uint8_t sprite_tiles_bit1[SPRITE_SLOTS];
static uint8_t sprite_attrib_latches[SPRITE_SLOTS]; // These contain the attribute bytes for the sprites.
static uint8_t sprite_x_counters[SPRITE_SLOTS];     // These contain the X positions for the sprites.
// Each sprite's counter is either "counting" (decremented every visible dot, rendering on or off) or "halted" (the
// sprite is output and its shifter shifts, only while rendering is on). Dot 339 with rendering on puts all eight into
// counting mode; with rendering off they stay as they were (usually halted), so the sprites draw as soon as
// rendering comes back (AccuracyCoin "Stale Sprite Shift Regs" / "Stale BG Shift Registers").
static bool sprite_counting[SPRITE_SLOTS];
// Fast path for the per-dot sprite work (ppu_generate_pixel): a slot whose shifters are empty outputs nothing, so it
// is skipped while it stays empty, and its counter is brought up to date in one step when something looks at it
// (sprite_sync(): before the sprite fetch reloads it, at dot 339, and for save states). Derived, not saved.
static uint64_t sprite_live;                 // bit n: slot n's shifters hold pixels (slots run dot by dot)
static uint32_t sprite_dots;                 // visible dots so far (wraps; only differences are used)
static uint32_t sprite_synced[SPRITE_SLOTS]; // sprite_dots up to which a skipped slot's counter is up to date
// Value on the internal OAM bus ("OAM copy buffer") during rendering: $FF while secondary OAM is cleared (dots 1-64),
// then each primary OAM byte as sprite evaluation reads it (dots 65-256). $2004 reads during rendering return this
// instead of OAM[OAMADDR] (nesdev PPU registers: OAMDATA); dots 257-340 and 0 return secondary OAM at oam2_addr.
static uint8_t oam_bus = 0xFF;

// Per-dot sprite evaluation state (nesdev "PPU sprite evaluation"; state machine as in Mesen's
// NesPpu::ProcessSpriteEvaluation). Everything runs from ppu_sprite_dot(), once per PPU dot.
static uint8_t oam2_addr;       // secondary OAM address; bit 5 set = secondary OAM full (writes disabled)
static bool spr_in_range;       // the sprite being copied is in range (copying its 4 bytes)
static bool spr_copy_done;      // all 64 sprites evaluated (OAMADDR wrapped): writes to OAM2 become reads
static bool spr0_added;         // the first sprite evaluated (dot 65/66) is in range: slot 0 is "sprite zero"
static bool spr0_visible;       // spr0_added latched during dots 257-320 for the next line's rendering
static int spr_overflow_bug;    // countdown for the sprite-overflow "diagonal" realignment
static bool spr_prev_rendering; // rendering state seen on the previous rendering-line dot
static uint8_t render_history;  // PPUMASK rendering-enabled bit per dot, bit 0 = the current dot
// PPU address bus as driven by the rendering fetches (nametable / attribute / pattern addresses). A $2007 read
// during rendering does not read v: its read-buffer fill happens a few dots later from whatever address the fetch
// cadence has on the bus at that moment (AccuracyCoin "$2007 Stress Test"; Mesen _ppuMemoryDataReadStateMachine).
static uint16_t ppu_bus_addr;
// PPU address line A12 as last driven on the bus (rendering fetches, or v for $2006/$2007 outside rendering). Each
// change is passed to the mapper with the time in dots: the MMC3 clocks its scanline counter on rising edges.
// A change from a rendering fetch reaches the mapper A12_DELAY_DOTS dots after the dot on which Mesen fetches (its
// IRQ then lines up with Mesen's: mmc3_test_2 4-scanline_timing, traces identical); a fetch runs here at the start
// of its dot, ahead of the CPU's view of it. Pending changes wait in a small queue (fetches come every 2 dots).
#define A12_DELAY_DOTS 2
static bool bus_a12;              // level of the last change (queued or delivered)
static uint64_t a12_due[4];       // queued changes: due time in dots; the level alternates, starting at !a12_head_level
static uint8_t a12_count;         // number of queued changes
static bool a12_head_level;       // level delivered last
static bool a12_src_sprite;       // the change being queued comes from a sprite pattern fetch
static bool a12_sprite[4];        // queued changes: from a sprite pattern fetch
static bool a12_delivered_sprite; // the change the mapper hook is being called for
static uint64_t ppu_dot_time(void)
{
    return ((uint64_t)g_frame_num * 262 + scanline) * 341 + sl_clock;
}
uint64_t ppu_dot_now(void)
{
    return ppu_dot_time();
}
bool ppu_a12_from_sprite(void)
{
    return a12_delivered_sprite;
}
uint32_t ppu_dots_skipped(void)
{
    return dots_skipped;
}
// A fetch (or the CPU's $2007 access) goes to a mapper that looks at the PPU bus (MMC5)
static inline void ppu_bus_watch(uint16_t addr, ppu_fetch_kind kind)
{
    if (mapper_watches_ppu) {
        mapper_ppu_fetch(addr, kind, ppu_dot_time());
    }
}
static void ppu_bus_a12_change(bool a12, unsigned delay)
{
    bus_a12 = a12;
    if ((0 == delay) && (0 == a12_count)) {
        a12_head_level = a12;
        a12_delivered_sprite = a12_src_sprite;
        mapper_ppu_a12(a12, ppu_dot_time());
    } else if (a12_count < 4) {
        a12_sprite[a12_count] = a12_src_sprite;
        a12_due[a12_count++] = ppu_dot_time() + delay;
        ppu_fast_dots = 0;
    }
}
// Called on every fetch: only a mapper that watches A12 (MMC3) pays for more than this test
static inline void ppu_bus_a12_delayed(uint16_t addr, unsigned delay)
{
    bool a12 = (addr & 0x1000) != 0;
    if (mapper_watches_a12 && (a12 != bus_a12)) {
        ppu_bus_a12_change(a12, delay);
    }
}
static void ppu_bus_a12(uint16_t addr)
{
    ppu_bus_a12_delayed(addr, A12_DELAY_DOTS);
}
// Deliver the queued A12 changes that are due (called every dot)
static inline void ppu_a12_catch_up(void)
{
    uint64_t now = ppu_dot_time();
    while ((a12_count > 0) && (a12_due[0] <= now)) {
        a12_head_level = !a12_head_level;
        a12_delivered_sprite = a12_sprite[0];
        mapper_ppu_a12(a12_head_level, a12_due[0]);
        a12_count--;
        memmove(&a12_due[0], &a12_due[1], a12_count * sizeof(a12_due[0]));
        memmove(&a12_sprite[0], &a12_sprite[1], a12_count * sizeof(a12_sprite[0]));
    }
}
static int ppudata_read_delay;   // dots until a rendering-time $2007 read fills the buffer; 0 = idle
static bool ppudata_inc_pending; // its v increment happens on the dot after the fill
#ifndef PPUDATA_READ_DOTS
#define PPUDATA_READ_DOTS 6
#endif
// A $2007 read whose strobe lands on the address dot of a background fetch: that fetch reads {its own high 6 address
// bits, the byte the previous fetch returned} instead of its own address, and the read buffer takes that byte too
// (AccuracyCoin "ALE + Read"). ppu_collide_address() makes the address.
static bool ppudata_collide;
static uint16_t ppu_collide_address(uint16_t address);
// A second $2006 write during rendering reaches v three dots after the dot it was made on (the copy from t happens at the
// end of the second dot after it; the fetch address follows v from the third). A fetch whose address dot came before
// but whose data dot is that third dot reads {the high 6 address bits from the new v, the low 8 from the old v}: a
// "hybrid" address (AccuracyCoin "Hybrid Addresses"; only nametable fetches are done, see ppu_v_copy_apply()).
#define V_COPY_DOTS 3
static int v_copy_delay; // dots until v takes v_copy_value; 0 = idle
static uint16_t v_copy_value;
static void ppu_v_copy_apply(void);

static void ppu_sprite_dot(uint32_t line, uint32_t dot);
static inline void ppu_sprite_dot_fast(uint32_t line, uint32_t dot);
void ppu_entry(void);
void ppu_run();
static inline void ppu_add_clocks(uint32_t cycles);
static void ppu_add_clocks_slow(uint32_t cycles);
static void ppu_vram_write(uint8_t value);
static void ppu_increment_vram_address();
static void ppu_increment_vram_addr_x_scroll();
static void ppu_increment_vram_addr_x_scroll_delayed();
static void ppu_increment_vram_addr_y_scroll();
static bool rendering_enabled();
static void ppu_render_scanline();

void ppu_init()
{
    /* Create (or re-create) the ppu cooperative thread */
    if (NULL != ppu_thread) {
        co_delete(ppu_thread);
    }
    ppu_thread = co_create(65536 * sizeof(void *), ppu_entry);
}

void ppu_power_on()
{
    // Reference: http://wiki.nesdev.com/w/index.php/PPU_power_up_state
    vram_latch_write = false;
    vram_latch = 0;
    vram_address = 0;
    PPUCTRL = 0;   // $2000
    PPUMASK = 0;   // $2001
    PPUSTATUS = 0; // $2002
    PPUSCROLL = 0; // $2005
    ppu_fine_x_scroll = 0;
    PPUADDR = 0; // $2006
    //  PPUDATA = 0; // $2007
    ppu_reset_flag = true;

    OAMADDR = 0;
    g_frame_num = 0;
    dots_skipped = 0;
    s0_hit_line = -1;
}

// Soft reset (the console's RESET button), modelled on Mesen: the PPU restarts with power-on timing (fresh coroutines,
// scanline 0 / dot 0, even frame) so the reset sequence lines up exactly like power-on. $2000/$2001 are cleared, the
// $2005/$2006 write latch and the read buffer are reset, and writes to $2000/$2001/$2005/$2006 are ignored until the
// end of the first frame again (nesdev PPU power up state). OAM, VRAM, palette and the VRAM address are kept.
static void ppu_update_nmi_line(void);

void ppu_soft_reset(void)
{
    ppu_init(); // recreate the PPU coroutine

    ppu_clock = 0;
    ppu_clock_requested = 0;
    ppu_fast_dots = 0;
    scanline = 0;
    sl_clock = 0;
    short_frame = false;
    skip_dot_this_frame = false;
    glb_skip_vblank = SKIP_FIRST_VBLANK;
    defer_cpu_switch = false;
    cpu_switch_owed = false;
    trace_bus_catchup = 0;
    trace_buffer_stale = false;
    g_frame_num = 0;
    dots_skipped = 0;
    s0_hit_line = -1;

    pattern_bit0_shift_reg = pattern_bit1_shift_reg = 0;
    attribute_bit0_shift_reg = attribute_bit1_shift_reg = 0;
    memset(sprite_tiles_bit0, 0, sizeof(sprite_tiles_bit0));
    memset(sprite_tiles_bit1, 0, sizeof(sprite_tiles_bit1));
    memset(sprite_attrib_latches, 0, sizeof(sprite_attrib_latches));
    memset(sprite_x_counters, 0, sizeof(sprite_x_counters));
    sprite_live = 0; // (all shifters empty)
    sprite_dots = 0;
    memset(sprite_synced, 0, sizeof(sprite_synced));
    memset(secondary_oam, 0xFF, sizeof(secondary_oam));
    oam_bus = 0xFF;
    oam2_addr = 0;
    spr_in_range = spr_copy_done = spr0_added = spr0_visible = false;
    spr_overflow_bug = 0;
    spr_prev_rendering = false;

    PPUCTRL = 0;
    PPUMASK = 0;
    PPUSCROLL = 0;
    ppu_fine_x_scroll = 0;
    vram_latch_write = false;
    ppudata_buffer = 0;
    ppu_reset_flag = true;
    io_latch = 0; // open bus and its decay timer restart too (ram_after_reset vs Mesen)
    open_bus_frame = 0;
    memset(open_bus_stamp, 0, sizeof(open_bus_stamp));
    ppu_update_nmi_line();
}

void ppu_reset()
{
    vram_latch_write = false;
    vram_latch = 0;
    vram_address = 0;
    scanline = SCANLINE_START;
    glb_skip_vblank = SKIP_FIRST_VBLANK;

    nmi_occurred = 0;
    pattern_bit0_shift_reg = 0;
    pattern_bit1_shift_reg = 0;
    attribute_bit0_shift_reg = 0;
    attribute_bit1_shift_reg = 0;
    io_latch = 0;
    open_bus_frame = 0;
    memset(open_bus_stamp, 0, sizeof(open_bus_stamp));

    ppu_clock = 0;
    ppu_clock_requested = 0;
    g_frame_num = 0;
    dots_skipped = 0;
    s0_hit_line = -1;
    ppu_reset_flag = true;
}

// Scanlines 0-239 (the picture), then the idle line 240
static void ppu_lines_0_to_240(void)
{
    uint32_t i, j;
    // render scanlines 0-239
    assert(sl_clock == 0);
    scanline = 0;
    for (i = 0; i < 240; i++) {
        ppu_render_scanline();
    }
    video_update_screen();

    assert(scanline == 240);
    assert(sl_clock == 0);
    // render scanline 240
    for (j = 0; j < 341; j++) {
        ppu_add_clocks(PPU_ONE_CYCLE);
    }
}

// Scanlines 241-260 (vertical blank) and the pre-render line 261
static void ppu_lines_241_to_261(void)
{
    uint32_t i, j;
    // scanline 241
    assert(scanline == 241);
    assert(sl_clock == 0);

    ppu_add_clocks(PPU_ONE_CYCLE);
    ppu_add_clocks(PPU_ONE_CYCLE);
    assert(sl_clock == 2);
    // nmi occurs on clock 1 (2nd clock) of scanline 241

    // race condition, see: https://wiki.nesdev.com/w/index.php/NMI
    // (nmi_occurred itself is now set inside ppu_add_clocks() at sl_clock==1, above;
    //  this keeps the NMI *request* at its previously-tuned timing, one cycle later,
    //  until that can be checked against a dedicated NMI-timing test)
    // (NMI request now raised in ppu_add_clocks() at dot 1, together with the VBL flag.)

    glb_skip_vblank = false; // flag indicates skip vblank due to race condition read of $2002
    //glb_skip_nmi = false;

    for (i = 0; i < PPU_CLOCKS_PER_SCANLINE - 2; i++) {
        ppu_add_clocks(PPU_ONE_CYCLE);
    }

    assert(scanline == 242);
    assert(sl_clock == 0);
    // render scanlines 242-260 (vblank)
    for (i = 0; i < 19; i++) {
        for (j = 0; j < 341; j++) {
            ppu_add_clocks(PPU_ONE_CYCLE);
        }
    }
    // Sprite Overflow / Sprite 0 hit are now cleared inside ppu_add_clocks() at dot 0 of scanline 261
    // (http://forums.nesdev.com/viewtopic.php?p=11417)

    // render scanline 261
    assert(scanline == 261);
    assert(sl_clock == 0);
    ppu_reset_flag = false;
    ppu_render_scanline();
    scanline = 0;
    short_frame = !short_frame; // toggle even/odd (long/short) frame
    if (skip_dot_this_frame) {
        dots_skipped++;
    }
    g_frame_num++;
}

// Reference: http://wiki.nesdev.com/w/index.php/PPU_rendering
void ppu_entry(void)
{
    sl_clock = 0;
#if SCANLINE_START == 0
    assert(scanline == 0);
    ppu_lines_0_to_240();
#endif
    for (;;) {
        ppu_lines_241_to_261();
        ppu_lines_0_to_240();
    }
}


void ppu_set_apu_log(bool active)
{
    apu_log_active = active;
}

#ifdef FEATURE_PPU_CATCHUP
// How many PPU clocks the CPU may run ahead of the PPU, from where the PPU is parked now, without the PPU running
// (see cpu.c). Until one CPU cycle plus a margin before the first of:
//  - the next dot at which the PPU changes the /NMI line (VBL set at 241/1, cleared at 261/1);
//  - the mapper's IRQ (MMC3): only while the picture is drawn does A12 move, and then a counted rise comes at most
//    once per line (8x8 sprites: the A12 low stretch longer than 10 dots that a rise needs exists once per line) or,
//    with 8x16 sprites, which can switch pattern tables from sprite to sprite, at most 6 times (the rise that ends
//    the background's low stretch and one per 13 dots of the sprite fetches). So the mapper's
//    a12_rises_to_irq() rises need at least ceil(rises / that) - 1 whole lines from the end of this one.
// All of it is conservative; vblank lines (no rises) are counted like the others.
int32_t ppu_clock_debt(void)
{
    return ppu_clock_requested;
}

int32_t ppu_catchup_limit(void)
{
    const int32_t e1 = 241 * 341 + 1, e2 = 261 * 341 + 1, frame = 262 * 341;
    int32_t pos = (int32_t)scanline * 341 + (int32_t)sl_clock;
    int32_t to = pos < e1 ? e1 - pos : pos < e2 ? e2 - pos : frame - pos + e1;
    int32_t limit = to * 4 - 24;
    if (a12_count > 0 || trace_bus_catchup > 0 || ppudata_inc_pending || ppudata_read_delay != 0 || v_copy_delay != 0) {
        return 0; // a change of A12 (a PPUDATA access) is on its way to the mapper: its IRQ comes a few dots from now
    }
    if (mapper_watches_ppu && rendering_enabled()) {
        // the MMC5's scanline IRQ: a count of lines starts at dot 1 of every line the picture is fetched on, so the n-th one from
        // now comes at least n - 1 whole lines away (its flag, in-frame and ExRAM writes are seen by the CPU only through a flush)
        uint32_t lines = mapper_lines_to_irq();
        if (lines != UINT32_MAX) {
            int32_t dots = (int32_t)(lines > 0 ? lines - 1 : 0) * 341 - (int32_t)sl_clock - 12;
            int32_t clocks = (lines > 0) ? dots * 4 - 24 : 0;
            if (clocks < limit) {
                limit = clocks;
            }
        }
    }
    if (mapper_watches_a12 && rendering_enabled()) {
        uint32_t rises = mapper_a12_rises_to_irq();
        if (rises != UINT32_MAX) {
            uint32_t per_line = (PPUCTRL & BIT_5) ? 6 : 1;
            uint32_t lines = (rises + per_line - 1) / per_line; // lines in which they could happen, at the least
            int32_t dots = (int32_t)(lines - 1) * 341 - (int32_t)sl_clock - 12;
            int32_t clocks = dots * 4 - 24;
            if (clocks < limit) {
                limit = clocks;
            }
        }
    }
    return limit;
}
#endif

bool ppu_add_cycles(uint32_t cycles)
{
    ppu_clock_requested += cycles;
    if (ppu_clock_requested > 0) {
        return true;
    }
    return false;
}

// The PPU's /NMI output is simply (VBL flag AND PPUCTRL.7). Call whenever either changes.
static void ppu_update_nmi_line(void)
{
    cpu_nmi_request(((nmi_occurred & BIT_7) != 0) && ((PPUCTRL & BIT_7) != 0));
}

// The end of ppu_add_clocks(), after the CPU has had its turn: next line after the last dot
static inline void ppu_end_of_line(void)
{
    if ((sl_clock == 341) || ((sl_clock == 340) && (scanline == 261) && skip_dot_this_frame)) {
        sl_clock = 0;
        scanline++;
    }
}

// A dot with nothing to process takes this short way (the work is in ppu_sprite_dot(), still per dot); everything else,
// and every dot at which ppu_fast_dots has run out, goes through ppu_add_clocks_slow(), which also works the count out.
static inline void ppu_add_clocks(uint32_t cycles)
{
    if (ppu_fast_dots > 0) {
        ppu_fast_dots--;
        ppu_clock += PPU_ONE_CYCLE;
        ppu_clock_requested -= PPU_ONE_CYCLE;
        sl_clock++;
        ppu_sprite_dot_fast(scanline, sl_clock);
        return;
    }
    ppu_add_clocks_slow(cycles);
}

// Lines on which no dot before the last two has an event: 0-238, 240, 242-259 (239, 241, 260 and 261 have dots with one)
static inline bool ppu_plain_line(uint32_t line)
{
    return line < 239 || line == 240 || (line >= 242 && line <= 259);
}

static inline void ppu_fast_dots_update(void)
{
    ppu_fast_dots = 0;
    if (a12_count > 0 || trace_bus_catchup > 0 || cpu_switch_owed || ppudata_inc_pending || ppudata_read_delay != 0 ||
        v_copy_delay != 0 || !ppu_plain_line(scanline) || sl_clock >= 339) {
        return;
    }
    int32_t by_cpu = ppu_clock_requested / PPU_ONE_CYCLE - 1; // the CPU's turn comes when the request is used up
    int32_t by_line = 338 - (int32_t)sl_clock;                // dots 340 and 341 (the end of the line) are slow ones
    int32_t n = by_cpu < by_line ? by_cpu : by_line;
    ppu_fast_dots = n > 0 ? n : 0;
}

static void ppu_add_clocks_slow(uint32_t cycles)
{
    if (a12_count > 0) {
        ppu_a12_catch_up();
    }
    if (trace_bus_catchup > 0) {
        if (--trace_bus_catchup == 0) {
            trace_note_bus_addr(vram_address);
            trace_buffer_stale = false;
        }
    }

    if (cpu_switch_owed) { // CPU observes the dot we deferred, now that its work is complete
        cpu_switch_owed = false;
        if ((ppu_clock_requested <= 0) && !cpu_ppu_turn()) {
            co_switch(cpu_thread);
        }
    }

    ppu_clock += cycles;
    ppu_clock_requested -= cycles;
    sl_clock += (cycles / PPU_ONE_CYCLE);

    assert(sl_clock <= 341);

    // Set VBLANK on clock 1 of scanline 241 (see: https://wiki.nesdev.com/w/index.php/PPU_registers#PPUSTATUS),
    // BEFORE the possible co_switch(cpu_thread) below hands control to the CPU. The previous placement of this
    // assignment, in ppu_entry() after two ppu_add_clocks() calls had already returned, only ran once the CPU
    // had already had a full turn at sl_clock==1 -- so a $2002 read landing there saw the flag one cycle late
    // (first visible at sl_clock==3 instead of ==1, confirmed against Mesen's trace of this exact ROM).
    if ((scanline == 241) && (sl_clock == 1) && (false == glb_skip_vblank)) {
        nmi_occurred = BIT_7;  // See: http://wiki.nesdev.com/w/index.php/NMI
        ppu_update_nmi_line(); // /NMI asserts together with the VBL flag (if enabled) -- ppu_vbl_nmi/04 vs Mesen
    }

    // Clear Sprite 0 hit and Sprite Overflow at dot 0 of the pre-render line (scanline 261), BEFORE the CPU observes
    // that dot. sl_clock==341 on scanline 260 *is* dot 0 of 261 (the wrap happens after the co_switch below).
    // Mesen: a $2002 read landing on this dot already sees both flags clear, while VBL is still set.
    if ((scanline == 260) && (sl_clock == 341)) {
        PPUSTATUS &= ~(BIT_5 | BIT_6);
        script_event(SCRIPT_EVENT_START_FRAME); // scripts: the frame starts with the pre-render line
    }

    // The APU state log (--apu-log) is written at the point where Mesen's end-of-frame event sees the CPU cycle
    // counter: 3 dots before scanline 240 starts (found by matching the cycle numbers of Mesen's apu_log.lua).
    // Scripts' endFrame event at the same point (script.h).
    if ((apu_log_active || script_active) && (scanline == 239) && (sl_clock == 338)) {
        if (apu_log_active) {
            rp2a03_frame_end();
        }
        script_event(SCRIPT_EVENT_END_FRAME);
    }

    // Clear VBLANK at dot 1 of the pre-render line, before the CPU observes that dot (hardware + Mesen:
    // a $2002 read landing on 261/1 already reads it clear -- ppu_vbl_nmi/03-vbl_clear_time).
    if ((scanline == 261) && (sl_clock == 1)) {
        nmi_occurred = 0;
        ppu_update_nmi_line(); // /NMI releases when VBL clears
    }

    if (v_copy_delay && (--v_copy_delay == 0)) {
        ppu_v_copy_apply();
    }

    if (ppudata_inc_pending) {
        ppudata_inc_pending = false;
        ppu_increment_vram_address();
    }
    if (ppudata_read_delay && (--ppudata_read_delay == 0)) {
        uint16_t a = ppu_bus_addr & 0x3FFF;
        // the next background fetch starts on this dot (odd dots 1-255 and 321-335): the read collides with it
        bool collide = (sl_clock & 1) && (sl_clock <= 255 || (sl_clock >= 321 && sl_clock <= 335)) &&
                       rendering_enabled() && ((scanline < 240) || (scanline == 261));
        if (collide) {
            ppudata_collide = true; // ppu_fetch() fills the buffer from the fetch's hybrid address
        } else {
            if (a < 0x3F00) {
                ppu_bus_watch(a, PPU_FETCH_CPU);
            }
            ppudata_buffer = ppu_vram_read((a >= 0x3F00) ? (a & 0x2FFF) : a);
        }
        ppudata_inc_pending = true;
    }

    // Sprite evaluation / OAM address logic for the dot the PPU just reached (before the CPU observes it).
    if (sl_clock == 341) {
        ppu_sprite_dot_fast((scanline == 261) ? 0 : scanline + 1, 0);
    } else if ((scanline == 261) && (sl_clock == 340) && skip_dot_this_frame) {
        ppu_sprite_dot_fast(0, 0); // odd frame: dot 340 of the pre-render line is skipped
    } else {
        ppu_sprite_dot_fast(scanline, sl_clock);
    }

    if ((scanline == 261) && (sl_clock == 338)) {
        skip_dot_this_frame = short_frame && rendering_enabled();
    }

    // Open-bus decay frame counter ticks at dot 0 of line 240 (sl_clock==341 on 239): a $2002 read whose bus access is at dot 338 of
    // line 239 still sees the old bits (Mesen's trace of Super Spike V'Ball + Nintendo World Cup polls $2002 there), the next read does not.
    if ((scanline == 239) && (sl_clock == 341)) {
        open_bus_frame++;
    }

#if 1 // increment scanline after cpu switch to make nintendulator trace log happy


    //  if (sl_clock == sl_end_tick) {
    //sl_clock = 0;
    //scanline++;

    //  }

    if (ppu_clock_requested <= 0) {
        if (defer_cpu_switch) {
            assert(sl_clock < 341); // deferred mode is only used mid-scanline; wrap below must not be skipped past
            cpu_switch_owed = true;
        } else if (!cpu_ppu_turn()) {
            co_switch(cpu_thread);
        }
    }

    ppu_end_of_line();
    ppu_fast_dots_update();

#else
    if ((ppu_clock_requested <= 0) && !cpu_ppu_turn()) {
        co_switch(cpu_thread);
    }
    if (sl_clock == 341) {
        sl_clock = 0;
        scanline++;
    }
#endif
}

void ppu_cleanup()
{
    if (ppu_thread) {
        co_delete(ppu_thread);
        ppu_thread = NULL;
    }
}


// Drive the bits in 'mask' with 'value' (refreshing their decay stamps); let undriven bits decay if stale.
static void ppu_open_bus_set(uint8_t mask, uint8_t value)
{
    int i;
    for (i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (mask & bit) {
            io_latch = (io_latch & ~bit) | (value & bit);
            open_bus_stamp[i] = open_bus_frame;
        } else if ((open_bus_frame - open_bus_stamp[i]) > 3) {
            io_latch &= ~bit;
        }
    }
}

// A register read that drives the bits NOT in 'open_mask' with 'value'; the bits in 'open_mask' come from open bus.
static uint8_t ppu_open_bus_apply(uint8_t open_mask, uint8_t value)
{
    ppu_open_bus_set((uint8_t)~open_mask, value);
    return (value & (uint8_t)~open_mask) | (io_latch & open_mask);
}

// $2004 value: during rendering of lines 0-239 the internal OAM bus, otherwise OAM[OAMADDR].
static uint8_t ppu_oam_data_value(void)
{
    int dot = (int)sl_clock, line = (int)scanline;
    if (dot == 341) {
        dot = 0;
        line += 1;
    }
    if (line == 262) {
        line = 0;
    }
    if (rendering_enabled() && ((line <= 239) || (line == 261))) {
        if ((dot >= 257) || (dot == 0)) {
            return secondary_oam[oam2_addr & 0x1F]; // sprite fetch / idle: whatever OAM2 address points at
        }
        return oam_bus;
    }
    return obj_attrib_mem[OAMADDR];
}

#define NO_RECENT_2007_READ (UINT64_MAX - 15) // "no read just before": (now - it) is never 1
static uint64_t last_2007_read_cycle = NO_RECENT_2007_READ;
static uint8_t ppu_fetch(uint16_t address)
{
    if (ppudata_collide) {
        ppudata_collide = false;
        address = ppu_collide_address(address);
        ppudata_buffer = ppu_vram_read(address);
    }
    ppu_bus_addr = address;
    ppu_bus_a12(address);
    ppu_bus_watch(address, PPU_FETCH_BG);
    return ppu_vram_read(address);
}
// Sprite pattern fetch: it runs here at dot 261 (269, ...), where the hardware and Mesen fetch at dot 260
static uint8_t ppu_fetch_sprite(uint16_t address)
{
    ppu_bus_addr = address;
    a12_src_sprite = true;
    ppu_bus_a12_delayed(address, A12_DELAY_DOTS - 1);
    a12_src_sprite = false;
    ppu_bus_watch(address, PPU_FETCH_SPRITE);
    return ppu_vram_read(address);
}
uint8_t ppu_read(uint16_t address)
{
    uint8_t value = 0xFF;

    address &= 0x2007; // mirroring
    switch (address) {
    case 0x2002: {
        vram_latch_write = false; // reset to "first write"

        // todo: I'm not sure this eff_ stuff is needed at all
        int eff_scanline = scanline;
        int eff_sl_clock = sl_clock;
        if (eff_sl_clock > 340) {
            eff_sl_clock -= 341;
            eff_scanline += 1;
        }
        if (eff_scanline == 241) {
        }
        if ((eff_scanline == 241) && (eff_sl_clock == 0)) {
            // read 1 dot before VBL set: reads clear, and the flag (and so NMI) never sets this frame
            value = 0;
            glb_skip_vblank = true;
        } else {
            value = nmi_occurred; // return previous value of nmi_occurred in bit 7
        }
        // Reading clears VBL, which releases /NMI. A read just after VBL sets (dots 1-2) therefore cuts the /NMI
        // pulse short before the CPU's edge detector samples it -- no special case needed.
        nmi_occurred = 0;
        ppu_update_nmi_line();

        value = ppu_open_bus_apply(0x1F, (value & BIT_7) | (PPUSTATUS & 0x60)); // bits 0-4 are open bus
        break;
    }
    case 0x2004: value = ppu_open_bus_apply(0x00, ppu_oam_data_value()); break;
    case 0x2007: {
        uint64_t now = cpu_get_cycle();
        if ((now - last_2007_read_cycle) == 1) {
            // A $2007 read on the CPU cycle right after another $2007 read is ignored: no buffer update, no address
            // increment, the value is PPU open bus
            last_2007_read_cycle = NO_RECENT_2007_READ;
            value = ppu_open_bus_apply(0xFF, 0x00);
        } else {
            last_2007_read_cycle = now;
            trace_note_bus_access(vram_address);
            bool deferred_increment = false;
            {
                uint16_t bus_addr = vram_address & 0x3FFF; // v is 15 bits; the PPU address bus is 14
                if (rendering_enabled() && ((scanline < 240) || (scanline == 261)) && (bus_addr < 0x3F00)) {
                    value = ppu_open_bus_apply(0x00, ppudata_buffer);
                    ppudata_read_delay = PPUDATA_READ_DOTS; // buffer filled later from the rendering bus
                    ppu_fast_dots = 0;
                    deferred_increment = true; // ...and v is incremented after that
                } else if (bus_addr < 0x3F00) {
                    value = ppudata_buffer;
                    ppu_bus_watch(bus_addr, PPU_FETCH_CPU);
                    ppudata_buffer = ppu_vram_read(bus_addr);
                    if (bus_addr < 0x2000 && mapper_chr_is_open()) {
                        // Nothing drives the bus (mapper 185 with CHR-ROM off). Seicross's two reads, of $0700 and $0701, both
                        // return $01 in Mesen (its trace shows it in the buffer, too); the cause is not known
                        ppudata_buffer = 0x01;
                    }
                    value = ppu_open_bus_apply(0x00, value); // buffered read drives all 8 bits
                } else {
                    value = ppu_vram_read(bus_addr);
                    ppudata_buffer = ppu_vram_read(bus_addr & (~BIT_12));
                    value = ppu_open_bus_apply(0xC0, value & 0x3F); // palette drives bits 0-5; 6-7 are open bus
                }
            }
            if (!deferred_increment) {
                ppu_increment_vram_address();
            }
            trace_buffer_value = value; // trace: peek shows the returned value until the catch-up
            trace_buffer_stale = true;
        }
        break;
    }
    default:
        value = ppu_open_bus_apply(0xFF, 0x00); // write-only registers: every bit is open bus
        break;
    }
    return value;
}


// http://wiki.nesdev.com/w/index.php/The_skinny_on_NES_scrolling
void ppu_write(uint16_t address, uint8_t value)
{
    address &= 0x2007; // mirroring
    switch (address) {
    case 0x2000: {
        if (!ppu_reset_flag) {
            uint8_t prev_nmi_enabled = PPUCTRL & BIT_7;
            LOG_TRACE(PPU, "PPUCTRL ($2000) = %02X", value);
            PPUCTRL = value;
            if (mapper_watches_ppu) {
                mapper_ppu_ctrl_write(value);
            }
            vram_latch &= 0x73FF;                   // clear bits 10, 11, 15
            vram_latch |= ((PPUCTRL & 0x03) << 10); // base name table address

            if (PPUCTRL & BIT_2) {
                vram_increment = 32;
            } else {
                vram_increment = 1;
            }

            // transition from low to high
            // /NMI = VBL AND PPUCTRL.7: enabling NMI while VBL is set raises it (an immediate NMI); disabling drops it.
            (void)prev_nmi_enabled;
            ppu_update_nmi_line();
        }
    } break;
    case 0x2001:
        if (!ppu_reset_flag) {
            PPUMASK = value;
        }
        break;
    case 0x2002: // nothing to do
        break;
    case 0x2003:
        OAMADDR = value;
        LOG_TRACE(PPU, "OAMADDR ($2003) = %02X", value);
        break;
    case 0x2004:
        LOG_TRACE(PPU, "OAMDATA ($2004) = %02X at OAMADDR %02X", value, OAMADDR);

        // ignore OAM write if rendering (https://forums.nesdev.com/viewtopic.php?f=3&t=17542)
        if (rendering_enabled() && ((scanline < 240) || (scanline == 261))) {
            // "Writes to OAMDATA during rendering ... do not modify values in OAM, but do perform a glitchy increment
            // of OAMADDR, bumping only the high 6 bits" (nesdev PPU registers)
            OAMADDR = (OAMADDR + 4) & 0xFC;
        } else {
            if ((OAMADDR & 0x03) == 0x02) {
                // clear out the unused bits in "byte2" of OAM entries (http://wiki.nesdev.com/w/index.php/PPU_OAM)
                obj_attrib_mem[OAMADDR++] = (value & 0xE3);
            } else {
                obj_attrib_mem[OAMADDR++] = value;
            }
        }
        break;
    case 0x2005:
        if (!ppu_reset_flag) {
            if (vram_latch_write == false) {
                // first write
                vram_latch &= 0xFFE0; // clear out bits 0-4
                vram_latch |= ((uint16_t)(value & 0xF8) >> 3);
                ppu_fine_x_scroll = value & 0x07;
            } else {
                // second write
                vram_latch &= 0x0C1F; // clear out bits
                vram_latch |= ((uint16_t)(value & 0xF8) << 2);
                vram_latch |= ((uint16_t)(value & 0x07) << 12);
            }
            vram_latch_write = !vram_latch_write; // toggle latch
        }
        break;
    case 0x2006:
        if (!ppu_reset_flag) {
            if (vram_latch_write == false) {
                // first write
                vram_latch &= 0x00FF; // clear out bits 8-15
                vram_latch |= ((uint16_t)(value & 0x3F) << 8);
            } else {
                // second write
                vram_latch &= 0xFF00; // clear out bits 0-7
                vram_latch |= (uint16_t)value;
                if (rendering_enabled() && ((scanline < 240) || (scanline == 261))) {
                    v_copy_value = vram_latch;
                    v_copy_delay = V_COPY_DOTS;
                    ppu_fast_dots = 0;
                    trace_note_bus_access(vram_latch);
                } else {
                    vram_address = vram_latch;
                    trace_note_bus_access(vram_address);
                }
            }
            vram_latch_write = !vram_latch_write; // toggle latch
        }
        break;
    case 0x2007:
        trace_note_bus_access(vram_address);
        ppu_vram_write(value);
        ppu_increment_vram_address();
        break;
    default: LOG_WARN(PPU, "write to unsupported register %04X", address); break;
    }

    ppu_open_bus_set(0xFF, value); // any register write drives all 8 bits of the PPU data bus
}

static bool rendering_enabled()
{
    if (PPUMASK & (BIT_3 | BIT_4)) {
        return true;
    }
    return false;
}

static bool ppu_is_rendering()
{
    if (rendering_enabled()) {
        if ((scanline < 241) || (scanline == 261)) {
            return true;
        }
    }
    return false;
}


// this is ONLY for use by read/writes to $2007
static void ppu_increment_vram_address()
{
    if (ppu_is_rendering()) {
        // during rendering a $2007 access bumps v like the renderer does: coarse X and Y (nesdev PPU scrolling)
        ppu_increment_vram_addr_x_scroll();
        ppu_increment_vram_addr_y_scroll();
    } else {
        vram_address = (vram_address + vram_increment) & 0x7FFF; // 15-bit v register
    }
}

// The renderer's coarse X increment (every 8 dots) sees a $2001 write 2 dots after PPUMASK changes, like the Y increment at
// dot 256 and the sprite unit: render_history bit 3 = PPUMASK two dots before the dot the group ends on. Micro Machines turns
// rendering off at dot 327 of a line and writes the palette through $2006 / $2007: the increment at dot 328 still happens
// (Mesen's trace shows it; AccuracyCoin measured 2-3 dots of delay on hardware).
static void ppu_increment_vram_addr_x_scroll_delayed()
{
    if (!(render_history & 8)) {
        return;
    }
    if ((vram_address & 0x001F) == 31) { // if coarse X == 31
        vram_address &= ~0x001F;         // coarse X = 0
        vram_address ^= 0x0400;          // switch horizontal nametable
    } else {
        vram_address++; // increment coarse X
    }
}

static void ppu_increment_vram_addr_x_scroll()
{
    if (ppu_is_rendering()) {
        if ((vram_address & 0x001F) == 31) { // if coarse X == 31
            vram_address &= ~0x001F;         // coarse X = 0
            vram_address ^= 0x0400;          // switch horizontal nametable
        } else {
            vram_address++; // increment coarse X
        }
    }
}

// Code taken from "Y Increment": http://wiki.nesdev.com/w/index.php/The_skinny_on_NES_scrolling
static void ppu_increment_vram_addr_y_scroll()
{
    int coarse_y;
    if ((vram_address & 0x7000) != 0x7000) { // if fine Y < 7
        vram_address += 0x1000;              // increment fine Y
    } else {
        coarse_y = (vram_address & 0x03E0) >> 5; // save coarse Y
        vram_address &= 0x0C1F;                  // fine Y = 0, coarse Y = 0
        if (coarse_y == 29) {
            coarse_y = 0;           // coarse Y = 0 (wrap-around)
            vram_address ^= 0x0800; // switch vertical nametable
        } else if (coarse_y == 31) {
            coarse_y = 0; // coarse Y = 0, nametable not switched
        } else {
            coarse_y += 1;
        } // increment coarse Y
        vram_address |= (coarse_y << 5); // put coarse Y back into v
    }
}

// http://wiki.nesdev.com/w/index.php/PPU_registers
static uint8_t ppu_vram_read(uint16_t address)
{
    uint8_t value = 0xA5;

    if (address < 0x2000) {
        // cartridge access
        value = mapper_read_chr(address);
    } else if (address < 0x3F00) {
        // ciram access
        uint16_t addr;
        addr = mapper_get_ciram_addr(address);
        value = (addr < 0x800) ? ciram[addr] : mapper_read_nt(address); // (MMC5: ExRAM, fill mode)
    } else {
        // palette access

        // Addresses $3F10/$3F14/$3F18/$3F1C are mirrors of $3F00/$3F04/$3F08/$3F0C. (https://wiki.nesdev.com/w/index.php/PPU_palettes)
        // todo: maybe can do this without an 'if'
        if (0x10 == (address & 0x13)) {
            address &= 0x0F;
        } else {
            address &= 0x1F; // mirroring at 0x20 byte intervals
        }

        value = bg_palette[address];
        if (BIT_0 == (PPUMASK & BIT_0)) { // Greyscale enabled?
            value &= 0x30;                // restrict palette to just the grey column ($00, $10, $20, $30)
        }
    }
    return value;
}

static void ppu_vram_write(uint8_t value)
{
    uint16_t bus_addr = vram_address & 0x3FFF; // v is 15 bits; the PPU address bus is 14
    if (bus_addr < 0x2000) {
        // cartridge access
        mapper_write_chr(bus_addr, value);
    } else if (bus_addr < 0x3F00) {
        // ciram access
        uint16_t addr;
        addr = mapper_get_ciram_addr(bus_addr);
        if (addr < 0x800) {
            ciram[addr] = value;
        } else {
            mapper_write_nt(bus_addr, value);
        }
    } else {
        // background palette access
        uint16_t address = bus_addr;
        address &= 0x1F; // mirroring at 0x20 byte intervals
        if (0 == (address & 0x03)) {
            address &= ~BIT_4; // mirroring of backdrop color
        }
        bg_palette[address] = value & 0x3F; // only 6 bit values in Palette
    }
}


// Bring a skipped slot's counter up to date: what the visible dots since sprite_synced[n] would have done to it one
// by one (a counting counter goes down by one per dot; at 0 it halts instead). A live slot is always up to date.
static void sprite_sync(unsigned n)
{
    if (!(sprite_live & (1ull << n))) {
        uint32_t dots = sprite_dots - sprite_synced[n];
        if (sprite_counting[n] && dots > 0) {
            if (dots <= sprite_x_counters[n]) {
                sprite_x_counters[n] = (uint8_t)(sprite_x_counters[n] - dots);
            } else {
                sprite_x_counters[n] = 0;
                sprite_counting[n] = false;
            }
        }
    }
    sprite_synced[n] = sprite_dots;
}

static void sprite_sync_all(void)
{
    for (unsigned n = 0; n < SPRITE_SLOTS; n++) {
        sprite_sync(n);
    }
}

// After slot n's shifters were loaded (or the state was loaded): it runs dot by dot only while they hold pixels
static void sprite_set_live(unsigned n)
{
    if (sprite_tiles_bit0[n] | sprite_tiles_bit1[n]) {
        sprite_live |= 1ull << n;
    } else {
        sprite_live &= ~(1ull << n);
        sprite_synced[n] = sprite_dots;
    }
}

// "Disable Sprite Limit": the sprites of the line after the hardware's first eight go into the extra slots (8-63)
// at dot 257, when the hardware evaluation has finished, so they draw with the right priority. Not the hardware's
// business: no PPU time, no bus activity (the pattern bytes are read without telling the mapper), the overflow flag
// and secondary OAM stay as they are. A trace (-t) keeps the hardware's limit so that it matches the reference.
static unsigned sprite_extras_used; // extra slots loaded on the last line

// ONE_HARDWARE_SPRITES in the environment keeps the hardware's limit whatever the setting says: the tests and the
// regression run set it, so that a user's one.cfg cannot change what they compare.
bool ppu_sprite_limit_forced(void)
{
    return getenv("ONE_HARDWARE_SPRITES") != NULL;
}

void ppu_set_sprite_limit(bool disable)
{
    sprite_no_limit = disable && !ppu_sprite_limit_forced();
}

static void sprite_load_extras(void)
{
    bool unlimited = sprite_no_limit && !cpu_trace_active() && (scanline < 240) && rendering_enabled();
    if (!unlimited && (0 == sprite_extras_used)) {
        return;
    }
    for (unsigned i = 0; i < sprite_extras_used; i++) { // the last line's extras are gone
        unsigned slot = SPRITE_HW + i;
        sprite_sync(slot);
        sprite_tiles_bit0[slot] = sprite_tiles_bit1[slot] = 0;
        sprite_set_live(slot);
    }
    sprite_extras_used = 0;
    if (!unlimited) {
        return;
    }
    int size_mask = (PPUCTRL & BIT_5) ? 15 : 7;
    unsigned found = 0;
    for (unsigned n = 0; n < 64; n++) {
        const uint8_t *spr = &obj_attrib_mem[n * 4];
        int row = scanline - (int)spr[0];
        if ((row < 0) || (row > size_mask) || (found++ < SPRITE_HW)) {
            continue; // not on this line, or one of the hardware's eight
        }
        unsigned slot = SPRITE_HW + sprite_extras_used++;
        if (spr[2] & BIT_7) {
            row ^= size_mask; // vertical flip
        }
        uint16_t addr;
        if (PPUCTRL & BIT_5) { // 8x16: tile bit 0 selects the pattern table
            addr = (uint16_t)(((spr[1] & 0x01) << 12) | ((spr[1] & 0xFE) << 4) | ((row & 0x08) << 1) | (row & 0x07));
        } else {
            addr = (uint16_t)(((PPUCTRL & BIT_3) << 9) | (spr[1] << 4) | row);
        }
        sprite_sync(slot);
        sprite_attrib_latches[slot] = spr[2];
        sprite_x_counters[slot] = spr[3];
        sprite_counting[slot] = true;
        sprite_tiles_bit0[slot] = ppu_vram_read(addr);
        sprite_tiles_bit1[slot] = ppu_vram_read((uint16_t)(addr + 8));
        sprite_set_live(slot);
    }
}

//http://wiki.nesdev.com/w/index.php/PPU_rendering

static uint8_t ppu_generate_pixel(uint32_t x, uint32_t y)
{
    uint8_t bg_pixel = 0;
    uint8_t bg_pixel_low_bits; // bits relevant to transparency (bits 0 and 1)
    uint8_t hsv_color;
    uint32_t sprite_num;
    uint8_t sprite_pixel = 0;
    uint8_t first_non_transparent_pixel = 0; // 0 indicates "not found"
    uint8_t sprite_pixel_priority = 0;
    uint16_t palette_address;

    if ((x < 256) && (y < 240)) {

        /* Pick one pixel from each shift register to create a 4-bit pixel */
        /*   The bit is selected using the value of $2005's fine-X scroll field */
        bg_pixel = (pattern_bit0_shift_reg & (BIT_15 >> ppu_fine_x_scroll)) >> (15 - ppu_fine_x_scroll);
        bg_pixel |= (pattern_bit1_shift_reg & (BIT_15 >> ppu_fine_x_scroll)) >> (14 - ppu_fine_x_scroll);
        bg_pixel_low_bits = bg_pixel; // just bits 0 and 1 are relevant to transparency
        if ((x < 8) && (0 == (PPUMASK & BIT_1))) {
            bg_pixel_low_bits = 0; // background hidden in leftmost 8 pixels
        }
        bg_pixel |= (attribute_bit0_shift_reg & (BIT_15 >> ppu_fine_x_scroll)) >> (13 - ppu_fine_x_scroll);
        bg_pixel |= (attribute_bit1_shift_reg & (BIT_15 >> ppu_fine_x_scroll)) >> (12 - ppu_fine_x_scroll);


        // Only the slots with pixels in their shifters (sprite_live), in slot order: the others would output
        // transparent pixels, and their counters are caught up by sprite_sync() when it matters.
        sprite_dots++;
        for (uint64_t live = sprite_live; live != 0; live &= live - 1) {
            sprite_num = bits_ctz64(live);
            if (sprite_counting[sprite_num] && (0 == sprite_x_counters[sprite_num])) {
                sprite_counting[sprite_num] = false; // counter reached 0: halt it and start drawing the sprite
            }
            if (!sprite_counting[sprite_num]) {
                if (!rendering_enabled()) {
                    continue; // forced blank: the shifter does not shift, nothing is output
                }
                uint8_t sprite_pixel_low_bits;
                if (sprite_attrib_latches[sprite_num] & BIT_6) {
                    sprite_pixel = (sprite_tiles_bit0[sprite_num] &
                                    BIT_0); // flip horizontally by selecting LSB and shifting right
                    sprite_pixel |= (sprite_tiles_bit1[sprite_num] & BIT_0) << 1;
                    sprite_tiles_bit0[sprite_num] >>= 1;
                    sprite_tiles_bit1[sprite_num] >>= 1;
                } else {
                    sprite_pixel = (sprite_tiles_bit0[sprite_num] & BIT_7) >> 7; // no-flip: select MSB and shift left.
                    sprite_pixel |= (sprite_tiles_bit1[sprite_num] & BIT_7) >> 6;
                    sprite_tiles_bit0[sprite_num] <<= 1;
                    sprite_tiles_bit1[sprite_num] <<= 1;
                }
                if ((sprite_tiles_bit0[sprite_num] | sprite_tiles_bit1[sprite_num]) == 0) {
                    sprite_live &= ~(1ull << sprite_num); // shifted out: skipped from the next dot on
                    sprite_synced[sprite_num] = sprite_dots;
                }
                sprite_pixel_low_bits = sprite_pixel; // just bits 0 and 1 are relevant to transparency
                if ((x < 8) && (0 == (PPUMASK & BIT_2))) {
                    sprite_pixel_low_bits = 0; // sprites hidden in leftmost 8 pixels
                }
                sprite_pixel |= ((sprite_attrib_latches[sprite_num] & 0x03) << 2);

                if ((0 == first_non_transparent_pixel) && (0 != sprite_pixel_low_bits)) {
                    first_non_transparent_pixel = sprite_pixel;
                    sprite_pixel_priority = sprite_attrib_latches[sprite_num] & BIT_5;
                    if ((sprite_num == 0) && spr0_visible) {                    // Sprite0 hit occurs if...
                        if ((0 != bg_pixel_low_bits) &&                         // BG pixel was not transparent
                            ((BIT_3 | BIT_4) == (PPUMASK & (BIT_3 | BIT_4))) && // BG and Sprite Rendering are enabled
                            ((0x06 == (0x06 & PPUMASK)) ||
                             (x > 7)) && // left clipping disabled, or pixel isn't in leftmost position
                            (x != 255))  // x is not 255
                        {
                            if (0 == (PPUSTATUS & BIT_6)) { // (the first pixel of the hit)
                                LOG_TRACE(PPU, "sprite 0 hit at x=%d y=%d", x, y);
                                s0_hit_line = (int)y;
                                s0_hit_x = (int)x;
                                s0_hit_frame = g_frame_num;
                            }
                            PPUSTATUS |= BIT_6; // set Sprite 0 hit flag
                        }
                    }
                }

            } else {
                sprite_x_counters[sprite_num] =
                    sprite_x_counters[sprite_num] - 1; // counting: decrement every visible dot
            }
        }

        /* implements "Priority multiplexer decision table" from http://wiki.nesdev.com/w/index.php/PPU_rendering */
        if (0 == bg_pixel_low_bits) {
            if (0 == (first_non_transparent_pixel & 0x03)) {
                palette_address = 0x3F00; // draw universal backdrop color
            } else {
                palette_address = 0x3F10 + first_non_transparent_pixel; // draw sprite
            }
        } else {
            if (0 == (first_non_transparent_pixel & 0x03)) {
                palette_address = 0x3F00 + bg_pixel; // draw background
            } else {
                // bg_pixel non-zero
                if (0 == sprite_pixel_priority) {
                    palette_address = 0x3F10 + first_non_transparent_pixel; // draw sprite
                } else {
                    palette_address = 0x3F00 + bg_pixel; // draw background
                }
            }
        }

        // todo: all the logic above is negated by this - don't do both
        if (false == rendering_enabled()) {
            // I'm not sure this is sophisticated enough: https://forums.nesdev.com/viewtopic.php?f=2&t=15824
            palette_address = 0x3F00; // draw universal backdrop color
        }


        // Same result as ppu_vram_read(palette_address), without the generic address decode: palette_address is
        // always $3F00-$3F1F here, and the $3F10/$14/$18/$1C mirrors can't occur (sprite color 0 is transparent).
        hsv_color = bg_palette[palette_address & 0x1F] & ((PPUMASK & BIT_0) ? 0x30 : 0x3F); // greyscale: $x0 column
        video_draw_pixel(x, y, hsv_color);
    }

    // update all the background shift registers (they only shift while rendering is enabled)
    if (rendering_enabled()) {
        pattern_bit0_shift_reg <<= 1;
        pattern_bit1_shift_reg = (uint16_t)((pattern_bit1_shift_reg << 1) |
                                            1); // serial input: a 1 on the high plane (AccuracyCoin "BG Serial In")
        attribute_bit0_shift_reg <<= 1;
        attribute_bit1_shift_reg <<= 1;
    }

    defer_cpu_switch = true;
    ppu_add_clocks(PPU_ONE_CYCLE); // every pixel takes one cycle; CPU sees the new dot after the next pixel is done
    defer_cpu_switch = false;

    // (VBLANK is cleared at dot 1 of the pre-render line in ppu_add_clocks().)

    return bg_pixel;
}


// Rendering is on for sprite purposes on the visible lines and the pre-render line.
static bool sprite_line(uint32_t line)
{
    return (line <= 239) || (line == 261);
}

// OAM row corruption: copy OAM row 'src' (8 bytes) over row 'dst', and the same for secondary OAM bytes.
static void ppu_corrupt_oam_row(uint8_t src, uint8_t dst)
{
    if (src != dst) {
        memcpy(&obj_attrib_mem[dst << 3], &obj_attrib_mem[src << 3], 8);
        secondary_oam[dst] = secondary_oam[src];
    }
}

// OAM2 address bit 5 is the "OAM2 overflowed" flag: it is set when the 5-bit address wraps (secondary OAM full
// during evaluation, and every line at dot 321 after the sprite fetch), and while it is set the address does not
// increment. It is only cleared by the address resets, and those only clear it if rendering was on at dot 63
// (evaluation start), 255 (sprite fetch start) or 339 (next line). AccuracyCoin "Frozen OAM2 Increment".
static uint8_t render_eff_history; // effective (delayed) rendering state of the sprite unit per dot, bit 0 = now
static void oam2_inc(void)
{
    if (!(oam2_addr & 0x20)) {
        oam2_addr++;
    }
}
static void oam2_reset(void)
{
    // render_eff_history bit 2 = the dot two before this one (63 / 255 / 339)
    oam2_addr = (render_eff_history & 4) ? 0 : (oam2_addr & 0x20);
}

static void sprite_eval_start(void)
{
    spr0_added = false;
    spr_in_range = false;
    spr_copy_done = false;
    spr_overflow_bug = 0;
    oam2_reset();
}

// Even dots 66-256: act on the byte read on the previous (odd) dot. OAMADDR is the live evaluation pointer:
// n = OAMADDR >> 2 (sprite), m = OAMADDR & 3 (byte), so $2003 writes during evaluation redirect it.
static void sprite_eval_even(uint32_t line, uint32_t dot)
{
    uint8_t n = OAMADDR >> 2;
    uint8_t m = OAMADDR & 0x03;
    int height = (PPUCTRL & BIT_5) ? 16 : 8;
    bool in_range_now = ((int)line >= (int)oam_bus) && ((int)line < (int)oam_bus + height);

    if (spr_copy_done) {
        n = (n + 1) & 0x3F;
        oam_bus = secondary_oam[oam2_addr & 0x1F]; // OAM2 writes are disabled: they read instead
    } else {
        if (!spr_in_range && in_range_now) {
            spr_in_range = true;
        }
        if (oam2_addr < 0x20) {
            secondary_oam[oam2_addr] = oam_bus; // copy (the Y byte is copied even when out of range)
            if (spr_in_range) {
                if (dot == 66) {
                    spr0_added = true; // the first sprite evaluated is "sprite zero", whatever its OAM index
                }
                m++;
                oam2_addr++;
                if (m >= 4) {
                    n = (n + 1) & 0x3F;
                    m = 0;
                    if (n == 0) {
                        spr_copy_done = true;
                    }
                }
                if ((oam2_addr & 0x03) == 0) { // all 4 bytes copied
                    spr_in_range = false;
                    if ((m != 0) && !in_range_now) {
                        m = 0; // misaligned start: resync unless the last byte, taken as a Y, is also in range
                    }
                }
            } else {
                n = (n + 1) & 0x3F; // not on this line: next sprite
                m = 0;
                if (n == 0) {
                    spr_copy_done = true;
                }
            }
        } else {
            // secondary OAM full: look for overflow, with the hardware's diagonal-read bug
            oam_bus = secondary_oam[oam2_addr & 0x1F];
            if (spr_in_range) {
                PPUSTATUS |= BIT_5; // sprite overflow
                m++;
                if (m == 4) {
                    n = (n + 1) & 0x3F;
                    m = 0;
                }
                if (spr_overflow_bug == 0) {
                    spr_overflow_bug = 3;
                } else if (--spr_overflow_bug == 0) {
                    spr_copy_done = true; // realigns and stops after "fetching" that sprite
                    m = 0;
                }
            } else {
                n = (n + 1) & 0x3F; // the bug: n and m both increment
                m = (m + 1) & 0x03;
                if (n == 0) {
                    spr_copy_done = true;
                }
            }
        }
    }
    OAMADDR = (uint8_t)((n << 2) | (m & 0x03));
}

// Called once per PPU dot, when the PPU reaches (line, dot) and before the CPU can observe that dot.
static void ppu_sprite_dot(uint32_t line, uint32_t dot)
{
    // A $2001 write reaches the sprite logic 2 dots later than PPUMASK changes here (hardware: 2-3 PPU cycles after
    // the write, depending on CPU/PPU alignment). AccuracyCoin "Misaligned OAM2 Address" needs the disable to land
    // after dot 268 and the enable after dot 286; "OAM Corruption" then corrupts row 4 (the 2-cycle case).
    // Only the sprite unit uses the delayed value; background fetches still use PPUMASK directly.
    render_history = (uint8_t)((render_history << 1) | (rendering_enabled() ? 1 : 0));
    bool rendering = (render_history >> 2) & 1;
    render_eff_history = (uint8_t)((render_eff_history << 1) | (rendering ? 1 : 0));

    if (dot == 0) {
        if (line == 261) {
            // Pre-render line: "OAMADDR bug" -- the OAM row OAMADDR points at is copied over the row the OAM2
            // address selects (usually row 0; another row if rendering was switched off mid-line: AccuracyCoin
            // "OAM Corruption", nesdev PPU registers OAMADDR).
            if (rendering) {
                ppu_corrupt_oam_row(OAMADDR >> 3, oam2_addr & 0x1F);
            }
        } else if (line <= 240) {
            if (render_eff_history & 4) {
                oam2_addr = 0; // reset (and overflow flag cleared) if rendering at dot 339
            }
        }
    }

    if (!sprite_line(line)) {
        spr_prev_rendering = rendering;
        return;
    }

    if (rendering != spr_prev_rendering) {
        spr_prev_rendering = rendering;
        if (!rendering && (dot >= 65) && (dot <= 256)) {
            OAMADDR++; // disabling rendering during evaluation bumps the OAM address (Mesen; nesdev forum)
        }
    }
    if (!rendering) {
        return;
    }

    if ((dot >= 1) && (dot <= 256)) {
        if (line == 261) {
            return; // no evaluation on the pre-render line
        }
        if (dot <= 64) {
            oam_bus = 0xFF; // clear secondary OAM
            secondary_oam[oam2_addr & 0x1F] = 0xFF;
            if ((dot & 1) == 0) {
                oam2_inc();
            }
        } else if (dot & 1) {
            if (dot == 65) {
                sprite_eval_start();
            }
            oam_bus = obj_attrib_mem[OAMADDR]; // odd dot: read primary OAM
        } else {
            sprite_eval_even(line, dot);
        }
    } else if ((dot >= 257) && (dot <= 320)) {
        spr0_visible = spr0_added;
        OAMADDR = 0; // OAMADDR is cleared on each of dots 257-320
        if (((dot - 1) & 4) == 0) {
            oam2_inc(); // first 4 dots of each 8-dot sprite fetch
        }
        if (dot == 257) {
            oam2_reset();
        }
    } else if (dot == 321) {
        oam2_inc(); // wraps to $20: overflow flag set every line
    }
}

// ppu_sprite_dot() for the dots where it can be done in a few lines. While PPUMASK has not changed for 8 dots (the
// rendering history is all 1s or all 0s and agrees with it) the history shifts, the "rendering changed" check and
// the line check have nothing to do; what is left on a dot is one of a few small steps, done here. Everything else
// (a change in flight, dot 0, dot 65, the pre-render line's OAMADDR bug) is ppu_sprite_dot(), unchanged.
static inline void ppu_sprite_dot_fast(uint32_t line, uint32_t dot)
{
    bool on = rendering_enabled();
    uint8_t steady = on ? 0xFF : 0x00;
    if (render_history != steady || render_eff_history != steady || spr_prev_rendering != on || dot == 0) {
        ppu_sprite_dot(line, dot);
        return;
    }
    if (!on || !sprite_line(line)) {
        return; // (no effect on these dots)
    }
    if (dot <= 64) {
        if (line == 261) {
            return; // no evaluation on the pre-render line
        }
        oam_bus = 0xFF; // clear secondary OAM
        secondary_oam[oam2_addr & 0x1F] = 0xFF;
        if ((dot & 1) == 0) {
            oam2_inc();
        }
    } else if (dot <= 256) {
        if (line == 261) {
            return;
        }
        if (dot & 1) {
            if (dot == 65) {
                sprite_eval_start();
            }
            oam_bus = obj_attrib_mem[OAMADDR]; // odd dot: read primary OAM
        } else {
            sprite_eval_even(line, dot);
        }
    } else if (dot <= 320) {
        spr0_visible = spr0_added;
        OAMADDR = 0; // OAMADDR is cleared on each of dots 257-320
        if (((dot - 1) & 4) == 0) {
            oam2_inc(); // first 4 dots of each 8-dot sprite fetch
        }
        if (dot == 257) {
            oam2_reset();
        }
    } else if (dot == 321) {
        oam2_inc(); // wraps to $20: overflow flag set every line
    }
}

#define ATTRIB_TABLE_ADDR(a) (0x23C0 | ((a) & 0x0C00) | (((a) & 0x0380) >> 4) | (((a) & 0x001C) >> 2))

// Background fetch latches keep their values while rendering is off: no fetches, no shift-register reloads and
// no shifting happen then, so re-enabling rendering mid-line draws the stale data (AccuracyCoin "Stale BG Shift
// Registers"; Mesen LoadTileInfo / ShiftTileRegisters). (File scope rather than static in ppu_render_scanline(),
// so that ppu_state() can save them.)
static uint8_t pattern_table_byte0; // holds bit 0 of 8 consecutive pixels
static uint8_t pattern_table_byte1; // holds bit 1 of 8 consecutive pixels
static uint8_t name_table_byte;
static uint8_t attrib_table_byte;
static uint8_t attrib_bits;
static uint16_t pattern_tbl_addr;
// The pattern fetch's address: the tile and table latched at the nametable fetch, the fine Y of v as it is at the
// pattern fetch itself (a $2006 write between the two changes it: AccuracyCoin "Hybrid Addresses")
#define PATTERN_ADDR(plane) ((uint16_t)((pattern_tbl_addr & ~7u) | (((vram_address >> 12) & 7u) + (plane))))

// The address a background fetch starting on this (odd) dot reads when a $2007 read collides with it: its own high 6 bits and,
// as the low byte, the byte the fetch before it returned.
static uint16_t ppu_collide_address(uint16_t address)
{
    uint8_t previous;
    switch (sl_clock & 7) {
    case 3: previous = name_table_byte; break;
    case 5: previous = attrib_table_byte; break;
    case 7: previous = pattern_table_byte0; break;
    default: previous = pattern_table_byte1; break;
    }
    return (uint16_t)((address & 0x3F00) | previous);
}

// The new v of a $2006 write arrives (see V_COPY_DOTS). If this dot is the data dot of a background nametable fetch, that
// fetch read a hybrid address: its high 6 bits from the new v, its low byte from the old one.
static void ppu_v_copy_apply(void)
{
    uint16_t old_v = vram_address;
    vram_address = v_copy_value;
    uint32_t address_dot = sl_clock - 1;
    if (((address_dot & 7) == 1) && (address_dot <= 249 || (address_dot >= 321 && address_dot <= 329)) &&
        rendering_enabled()) {
        name_table_byte = ppu_fetch((uint16_t)(0x2000 | (v_copy_value & 0x0F00) | (old_v & 0x00FF)));
        pattern_tbl_addr = (name_table_byte << 4) | ((PPUCTRL & BIT_4) << 8) | ((old_v & 0x7000) >> 12);
    }
}


static void ppu_render_scanline()
{
    uint32_t oam_index = 0;
    uint32_t sprite_index = 0;

    assert(sl_clock == 0);
    defer_cpu_switch = true;
    ppu_add_clocks(PPU_ONE_CYCLE); // clock 0 (deferred: CPU observes dot 1 after pixel 0 is generated)
    defer_cpu_switch = false;

    assert(sl_clock == 1);
    while (sl_clock <= 256) {
        if (rendering_enabled()) {
            name_table_byte = ppu_fetch(0x2000 | (vram_address & 0x0FFF));
            pattern_tbl_addr = (name_table_byte << 4) | ((PPUCTRL & BIT_4) << 8) | ((vram_address & 0x7000) >> 12);
        }
        ppu_generate_pixel(sl_clock - 1,
                           scanline); // todo: just track x separately so I don't have subtract 1 every time
        ppu_generate_pixel(sl_clock - 1, scanline);

        if (rendering_enabled()) {
            attrib_table_byte = ppu_fetch(ATTRIB_TABLE_ADDR(vram_address));
            attrib_bits = (attrib_table_byte >> (((vram_address & 0x40) >> 4) | (vram_address & 0x02))) & 0x03;
        }
        //        attrib_bits = (attrib_table_byte >> (((vram_address & BIT_0) << 1) | ((vram_address & BIT_5) >> 3))) & 0x03;
        ppu_generate_pixel(sl_clock - 1, scanline);
        ppu_generate_pixel(sl_clock - 1, scanline);

        // (pattern address latched at the nametable fetch: fine Y and $2000.4 as they were then)
        if (rendering_enabled())
            pattern_table_byte0 = ppu_fetch(PATTERN_ADDR(0));
        ppu_generate_pixel(sl_clock - 1, scanline);
        ppu_generate_pixel(sl_clock - 1, scanline);

        if (rendering_enabled())
            pattern_table_byte1 = ppu_fetch(PATTERN_ADDR(8));
        ppu_generate_pixel(sl_clock - 1, scanline);
        ppu_generate_pixel(sl_clock - 1, scanline);

        // every 8 cycles do this stuff:
        ppu_increment_vram_addr_x_scroll_delayed();
        if (render_history & 8) {
            // reload the low 8 bits of the shift registers (the reload sees a $2001 change 2 dots late, like the X
            // increment; with it skipped, the serial input below is what the next 8 pixels show)
            pattern_bit0_shift_reg = (pattern_bit0_shift_reg & 0xFF00) | pattern_table_byte0;
            pattern_bit1_shift_reg = (pattern_bit1_shift_reg & 0xFF00) | pattern_table_byte1;
            attribute_bit0_shift_reg = (attribute_bit0_shift_reg & 0xFF00) | ((attrib_bits & BIT_0) ? 0x00FF : 0);
            attribute_bit1_shift_reg = (attribute_bit1_shift_reg & 0xFF00) | ((attrib_bits & BIT_1) ? 0x00FF : 0);
        }
    }

    // clock 257
    assert(sl_clock == 257);
    uint16_t v_before_copy = vram_address; // (after the Y increment) the first garbage NT fetch at 257 still uses it
    // The Y increment belongs to dot 256 and, like the sprite unit, sees a $2001 change 2 dots after PPUMASK
    // (render_history bit 3 = PPUMASK at dot 254). Rendering enabled "on dot 256" must not increment Y
    // (AccuracyCoin "Frozen OAM2 Increment"; also "$2007 Stress Test" frame 188).
    if (render_history & 8) {
        ppu_increment_vram_addr_y_scroll();
        v_before_copy = vram_address;
    }
    if (rendering_enabled()) {

        // copy horizontal position from Loopy T to Loopy V (vram_latch to vram_address)
        // technically this should occur on clock 257
        vram_address &= ~0x041F;
        vram_address |= (vram_latch & 0x041F);
    }

    oam_index = 0;
    sprite_index = 0;
    sprite_load_extras();
    while (sl_clock <= 320) {
        // garbage NT byte = read from vram
        //ppu_add_clocks(PPU_TWO_CYCLES);
        // garbage nametable fetches (the one at dot 257 happens before the horizontal copy from t)
        ppu_bus_addr = 0x2000 | (((sprite_index == 0) ? v_before_copy : vram_address) & 0x0FFF);
        if (rendering_enabled()) {
            ppu_bus_a12(ppu_bus_addr);
            ppu_bus_watch(ppu_bus_addr, PPU_FETCH_IDLE);
        }
        ppu_add_clocks(PPU_ONE_CYCLE);
        ppu_add_clocks(PPU_ONE_CYCLE);
        // garbage NT byte = read from vram
        //ppu_add_clocks(PPU_TWO_CYCLES);  // don't add two cycles at once!  It will prevent CPU<->PPU switching from occuring properly
        ppu_bus_addr = 0x2000 | (vram_address & 0x0FFF);
        if (rendering_enabled()) {
            ppu_bus_watch(ppu_bus_addr, PPU_FETCH_IDLE);
        }
        ppu_add_clocks(PPU_ONE_CYCLE);
        ppu_add_clocks(PPU_ONE_CYCLE);

        {
            // In-range check and row, like the evaluator but on the 8-bit line number: the pre-render line counts as
            // line 261 & 255 = 5, so sprites left in secondary OAM can be drawn on line 0 (AccuracyCoin "Sprites On
            // Scanline 0", https://forums.nesdev.org/viewtopic.php?t=26291). Out-of-range entries fetch transparent.
            uint16_t spr_pattern_addr;
            uint8_t spr_byte0, spr_byte1;
            uint8_t spr_y = secondary_oam[oam_index];
            uint8_t spr_tile = secondary_oam[oam_index + 1];
            uint8_t attrib = secondary_oam[oam_index + 2];
            uint8_t spr_x = secondary_oam[oam_index + 3];
            if (oam2_addr & 0x20) {
                // OAM2 address frozen (overflow flag still set): every fetch step reads the same OAM2 byte
                spr_y = spr_tile = attrib = spr_x = secondary_oam[oam2_addr & 0x1F];
            }
            int size_mask = (PPUCTRL & BIT_5) ? 15 : 7;
            int row = (int)(scanline & 0xFF) - (int)spr_y;
            bool in_range = (row >= 0) && (row <= size_mask);
            row &= size_mask;
            if (attrib & BIT_7)
                row ^= size_mask;  // vertical flip (over all 16 rows for 8x16)
            if (PPUCTRL & BIT_5) { // 8x16: tile bit 0 selects the pattern table
                spr_pattern_addr =
                    ((spr_tile & 0x01) << 12) | ((spr_tile & 0xFE) << 4) | ((row & 0x08) << 1) | (row & 0x07);
            } else { // 8x8: PPUCTRL bit 3 selects the pattern table
                spr_pattern_addr = ((PPUCTRL & BIT_3) << 9) | (spr_tile << 4) | row;
            }

            // The sprite's latches are only loaded while rendering is on: disabling rendering during the sprite
            // fetch leaves the old counter / shifter contents in place (AccuracyCoin "Stale Sprite Shift Regs" 6).
            bool load_latches = rendering_enabled();
            sprite_sync(sprite_index); // (its counter is about to change)
            if (load_latches) {
                sprite_attrib_latches[sprite_index] = attrib;
                sprite_x_counters[sprite_index] = spr_x;
            }

            spr_byte0 = load_latches ? ppu_fetch_sprite(spr_pattern_addr) : 0;
            ppu_add_clocks(PPU_ONE_CYCLE);
            ppu_add_clocks(PPU_ONE_CYCLE);

            load_latches = load_latches && rendering_enabled();
            spr_byte1 = load_latches ? ppu_fetch_sprite(spr_pattern_addr + 8) : 0;
            ppu_add_clocks(PPU_ONE_CYCLE);
            ppu_add_clocks(PPU_ONE_CYCLE);

            if (!load_latches) {
                // keep the stale shifter contents
            } else if (in_range) {
                sprite_tiles_bit0[sprite_index] = spr_byte0;
                sprite_tiles_bit1[sprite_index] = spr_byte1;
            } else {
                /* make this sprite transparent */
                sprite_tiles_bit0[sprite_index] = 0;
                sprite_tiles_bit1[sprite_index] = 0;
                sprite_attrib_latches[sprite_index] = 0;
                sprite_x_counters[sprite_index] = 0xFE;
            }
            sprite_set_live(sprite_index);
        }
        oam_index += 4; // move on to next sprite
        sprite_index++;

        if ((261 == scanline) && (sl_clock >= 280) && (rendering_enabled())) {
            // copy vertical position from Loopy T to Loopy V (vram_latch to vram_address)
            vram_address &= ~0x7BE0;
            vram_address |= (vram_latch & 0x7BE0);
        }
    }

#ifdef ONE_DEBUGGER
    // one-debugger's background viewer: the scroll the next picture line starts with (the copies from t are done,
    // and the next line's first tile fetch is about to use v)
    if ((scanline == 261) || (scanline < 239)) {
        ppu_line_scroll *line = &line_scroll[(scanline == 261) ? 0 : scanline + 1];
        line->v = vram_address;
        line->fine_x = ppu_fine_x_scroll;
        line->mask = PPUMASK;
        line->ctrl = PPUCTRL;
        for (int page = 0; page < 4; page++) {
            line->bg_chr[page] = mapper_debug_chr_offset((uint16_t)(((PPUCTRL & BIT_4) << 8) | (page << 10)));
        }
    }
#endif

    // clock 321
    while (sl_clock <= 336) {
        if (rendering_enabled()) {
            name_table_byte = ppu_fetch(0x2000 | (vram_address & 0x0FFF));
            pattern_tbl_addr = (name_table_byte << 4) | ((PPUCTRL & BIT_4) << 8) | ((vram_address & 0x7000) >> 12);
        }
        ppu_generate_pixel(sl_clock - 1, scanline);
        ppu_generate_pixel(sl_clock - 1, scanline);

        if (rendering_enabled()) {
            attrib_table_byte = ppu_fetch(ATTRIB_TABLE_ADDR(vram_address));
            attrib_bits = (attrib_table_byte >> (((vram_address & 0x40) >> 4) | (vram_address & 0x02))) & 0x03;
        }
        //attrib_bits = (attrib_table_byte >> (((vram_address & BIT_0) << 1) | ((vram_address & BIT_5) >> 3))) & 0x03;
        ppu_generate_pixel(sl_clock - 1, scanline);
        ppu_generate_pixel(sl_clock - 1, scanline);

        // (pattern address latched at the nametable fetch: fine Y and $2000.4 as they were then)
        if (rendering_enabled())
            pattern_table_byte0 = ppu_fetch(PATTERN_ADDR(0));
        ppu_generate_pixel(sl_clock - 1, scanline);
        ppu_generate_pixel(sl_clock - 1, scanline);

        if (rendering_enabled())
            pattern_table_byte1 = ppu_fetch(PATTERN_ADDR(8));
        ppu_generate_pixel(sl_clock - 1, scanline);
        ppu_generate_pixel(sl_clock - 1, scanline);


        // every 8 cycles do this stuff:
        ppu_increment_vram_addr_x_scroll_delayed();
        if (render_history & 8) {
            // reload the low 8 bits of the shift registers (the reload sees a $2001 change 2 dots late, like the X
            // increment; with it skipped, the serial input below is what the next 8 pixels show)
            pattern_bit0_shift_reg = (pattern_bit0_shift_reg & 0xFF00) | pattern_table_byte0;
            pattern_bit1_shift_reg = (pattern_bit1_shift_reg & 0xFF00) | pattern_table_byte1;
            attribute_bit0_shift_reg = (attribute_bit0_shift_reg & 0xFF00) | ((attrib_bits & BIT_0) ? 0x00FF : 0);
            attribute_bit1_shift_reg = (attribute_bit1_shift_reg & 0xFF00) | ((attrib_bits & BIT_1) ? 0x00FF : 0);
        }
    }

    //clock 337
    // name_table_byte = read from vram
    //ppu_add_clocks(PPU_TWO_CYCLES);
    ppu_bus_addr = 0x2000 | (vram_address & 0x0FFF); // the two unused nametable fetches
    if (rendering_enabled()) {
        ppu_bus_a12(ppu_bus_addr);
        ppu_bus_watch(ppu_bus_addr, PPU_FETCH_IDLE);
    }
    ppu_add_clocks(PPU_ONE_CYCLE); // clock 337
    ppu_add_clocks(PPU_ONE_CYCLE); // clock 338
    if (rendering_enabled()) {
        ppu_bus_watch(ppu_bus_addr, PPU_FETCH_IDLE); // (the second of the two)
    }

    if (render_history & 4) {
        // dot 339: the sprite counters loaded during 257-320 start counting (see sprite_counting). Like the
        // sprite evaluation, this sees a $2001 change 2 dots after PPUMASK does.
        sprite_sync_all(); // (the counters' state before it changes)
        memset(sprite_counting, true, sizeof(sprite_counting));
    }
    ppu_add_clocks(PPU_ONE_CYCLE); // clock 339

    if ((scanline >= 261) && skip_dot_this_frame) {
        // odd frame with rendering: dot 340 skipped (already wrapped in ppu_add_clocks)
    } else {
        ppu_add_clocks(PPU_ONE_CYCLE); // clock 340
    }
}


// DEBUG

uint8_t ppu_debug_read(uint16_t address)
{
    if (address < 0x2000) {
        return mapper_debug_read_chr(address);
    } else if (address < 0x3F00) {
        uint16_t addr = mapper_get_ciram_addr(address);
        return (addr < 0x800) ? ciram[addr] : mapper_read_nt(address);
    } else {
        // palette access (the PPU's own mirroring: $3F10/$3F14/$3F18/$3F1C are $3F00/$3F04/$3F08/$3F0C)
        return ppu_debug_read_palette((uint8_t)address);
    }
}

// Side-effect-free peek of a PPU register, as a debugger/trace would see it (matches Mesen's PeekRam):
// write-only registers return the PPU open-bus latch; $2002 merges the status flags with the latch's low 5 bits;
// $2004 returns OAM[OAMADDR]; $2007 returns the read buffer (or palette data with open-bus bits 6-7).
uint8_t ppu_debug_read_regs(uint16_t address)
{
    uint8_t value;
    address &= 0x2007; // mirroring
    switch (address) {
    case 0x2002:
        value = (nmi_occurred & BIT_7) | (PPUSTATUS & 0x60) | (io_latch & 0x1F);
        if ((scanline == 241) && (sl_clock < 3)) {
            value &= ~BIT_7; // Mesen's peek hides VBL for dots 0-2 of line 241 (debugger view only, not real reads)
        }
        break;
    case 0x2004: value = ppu_oam_data_value(); break;
    case 0x2007: {
        uint16_t bus_addr = trace_bus_addr;
        uint8_t bus_palette = trace_bus_palette;
        if ((0 == trace_bus_catchup) && !ppu_is_rendering()) {
            // idle bus with rendering off: it shows v, which the renderer's last increments may have moved since the access
            // (Micro Machines switches rendering off 1-3 dots before the increment at dot 328)
            bus_addr = vram_address & 0x3FFF;
            bus_palette = (bus_addr >= 0x3F00) ? ppu_vram_read(bus_addr) : 0;
        }
        if (bus_addr >= 0x3F00) {
            value = (bus_palette & 0x3F) | (io_latch & 0xC0); // see trace_note_bus_access()
        } else {
            value = trace_buffer_stale ? trace_buffer_value : ppudata_buffer;
        }
        break;
    }
    default: // $2000, $2001, $2003, $2005, $2006 are write-only: reads return open bus
        value = io_latch;
        break;
    }
    return value;
}

// Trace labelling of the odd-frame skipped dot. This PPU skips dot 340 of the pre-render line; Mesen skips dot 339
// instead. Cycle timing is identical -- only the label differs -- so on a skipping frame, relabel 261/339 as
// 261/340 and 261/340 (the CPU's view of the dot that is really 0/0) as 0/0.
void ppu_debug_get_trace_position(int *trace_scanline, int *trace_dot)
{
    int sl = (int)scanline;
    int dot = (int)sl_clock;
    if (dot == 341) {
        // ppu_add_clocks() lets the CPU run at sl_clock==341 before wrapping; that is dot 0 of the next line
        sl += 1;
        dot = 0;
    }
    if ((sl == 261) && skip_dot_this_frame && (dot >= 339)) {
        if (dot == 339) {
            dot = 340;
        } else if (dot == 340) {
            sl = 0;
            dot = 0;
        }
    }
    if (sl >= 261) {
        sl -= 262; // pre-render line is shown as -1
    }
    *trace_scanline = sl;
    *trace_dot = dot;
}

// Every byte OAM DMA copies is a write to $2004, so it refreshes the PPU open-bus latch like any register write.
void ppu_set_open_bus(uint8_t value)
{
    ppu_open_bus_set(0xFF, value);
}

uint8_t ppu_debug_read_oam(uint8_t address)
{
    return obj_attrib_mem[address];
}

uint8_t ppu_get_ppustatus()
{
    uint8_t value = nmi_occurred; // return previous value of nmi_occurred in bit 7
    value |= ((PPUSTATUS & 0x60) | (0x1f & io_latch));
    return (value);
}

uint8_t ppu_get_ppuctrl()
{
    return PPUCTRL;
}
uint8_t ppu_get_ppumask()
{
    return PPUMASK;
}

uint16_t ppu_get_vram_addr()
{
    return vram_address;
}

uint16_t ppu_debug_get_v(void)
{
    return vram_address;
}

uint16_t ppu_debug_get_t(void)
{
    return vram_latch;
} // Loopy T
const ppu_line_scroll *ppu_debug_line_scroll(void)
{
    return line_scroll;
}

uint8_t ppu_debug_get_fine_x(void)
{
    return ppu_fine_x_scroll;
} // Loopy x
bool ppu_debug_get_w(void)
{
    return vram_latch_write;
} // Loopy w (true = second write)
// Scripts (script.c): OAM, secondary OAM and palette RAM written directly, without the side effects of $2004/$2007
void ppu_debug_write_oam(uint8_t address, uint8_t value)
{
    obj_attrib_mem[address] = value;
}

uint8_t ppu_debug_read_oam2(uint8_t address)
{
    return secondary_oam[address & 0x1F];
}

void ppu_debug_write_oam2(uint8_t address, uint8_t value)
{
    secondary_oam[address & 0x1F] = value;
}

void ppu_debug_write_palette(uint8_t index, uint8_t value)
{
    index &= 0x1F;
    if ((index & 0x13) == 0x10) {
        index &= 0x0F; // $3F10/$14/$18/$1C are mirrors of $3F00/$04/$08/$0C
    }
    bg_palette[index] = value & 0x3F;
}

// Palette RAM as the PPU reads it: $3F10/$3F14/$3F18/$3F1C are $3F00/$3F04/$3F08/$3F0C (ppu_vram_write() stores
// them there; their own slots of bg_palette keep their power-on values)
static uint8_t palette_slot(uint8_t index)
{
    index &= 0x1F;
    return ((index & 0x13) == 0x10) ? (uint8_t)(index & 0x0F) : index;
}

uint8_t ppu_debug_read_palette(uint8_t index)
{
    return bg_palette[palette_slot(index)];
}

uint32_t ppu_debug_get_clk()
{
    return ppu_clock;
}

bool ppu_debug_sprite0_hit(int *line, int *x, uint32_t *frame)
{
    *line = s0_hit_line;
    *x = s0_hit_x;
    *frame = s0_hit_frame;
    return s0_hit_line >= 0;
}

uint32_t ppu_debug_get_scanline()
{
    if (sl_clock == 341) {
        // there's a weird condition in ppu_add_clocks() where sl_clock gets incremented to 341, then rolled over to 0 - but in between the CPU runs.
        return scanline + 1;
    }
    return scanline;
}

uint32_t ppu_debug_get_scanline_clock()
{
    if (sl_clock == 341) {
        // there's a weird condition in ppu_add_clocks() where sl_clock gets incremented to 341, then rolled over to 0 - but in between the CPU runs.
        return 0;
    }
    return sl_clock;
}

uint8_t ppu_get_ppusar()
{
    return OAMADDR;
}

uint8_t ppu_get_ppusdr()
{
    // todo: maybe this is useless
    return 0x06;
}

uint32_t ppu_debug_get_frame_num()
{
    return g_frame_num;
}

// --- save states -------------------------------------------------------------------------------------------------
// A state is taken while the PPU counts the dots of the idle line 240 (ppu_state_quiet()): it is then suspended in
// ppu_add_clocks(), in the loop of ppu_lines_0_to_240(). A loaded state continues from there in a new coroutine:
// the end of that ppu_add_clocks(), the rest of line 240, then the normal frame loop.

bool ppu_state_quiet(void)
{
    return (scanline == 240) && (sl_clock >= 1) && !defer_cpu_switch && !cpu_switch_owed;
}

static void ppu_resume_entry(void)
{
    ppu_end_of_line();
    while (scanline == 240) {
        ppu_add_clocks(PPU_ONE_CYCLE);
    }
    for (;;) {
        ppu_lines_241_to_261();
        ppu_lines_0_to_240();
    }
}

void ppu_state_restart(void)
{
    if (ppu_thread != NULL) {
        co_delete(ppu_thread);
    }
    ppu_thread = co_create(65536 * sizeof(void *), ppu_resume_entry);
    ppu_fast_dots = 0;
}

void ppu_state(state_io *io)
{
    sprite_sync_all(); // the saved counters are the ones dot-by-dot emulation would have
    STATE(io, ppu_clock);
    STATE(io, ppu_clock_requested);
    STATE(io, ciram);
    STATE(io, bg_palette);
    STATE(io, obj_attrib_mem);
    STATE(io, secondary_oam);
    STATE(io, g_frame_num);
    STATE(io, dots_skipped);
    STATE(io, scanline);
    STATE(io, sl_clock);
    STATE(io, short_frame);
    STATE(io, skip_dot_this_frame);
    STATE(io, nmi_occurred);
    STATE(io, ppudata_buffer);
    STATE(io, io_latch);
    STATE(io, open_bus_frame);
    STATE(io, open_bus_stamp);
    STATE(io, trace_bus_addr);
    STATE(io, trace_bus_palette);
    STATE(io, trace_bus_catchup);
    STATE(io, trace_buffer_stale);
    STATE(io, trace_buffer_value);
    STATE(io, glb_skip_vblank);
    STATE_SKIP(ppu_fast_dots); // (work out again at the first slow dot)
    STATE(io, defer_cpu_switch);
    STATE(io, cpu_switch_owed);
    STATE(io, ppu_reset_flag);
    STATE(io, pattern_bit0_shift_reg);
    STATE(io, pattern_bit1_shift_reg);
    STATE(io, attribute_bit0_shift_reg);
    STATE(io, attribute_bit1_shift_reg);
    STATE(io, pattern_table_byte0);
    STATE(io, pattern_table_byte1);
    STATE(io, name_table_byte);
    STATE(io, attrib_table_byte);
    STATE(io, attrib_bits);
    STATE(io, pattern_tbl_addr);
    STATE(io, PPUCTRL);
    STATE(io, PPUMASK);
    STATE(io, PPUSTATUS);
    STATE(io, PPUADDR);
    STATE(io, PPUSCROLL);
    STATE(io, ppu_fine_x_scroll);
    STATE(io, vram_latch_write);
    STATE(io, vram_latch);
    STATE(io, vram_address);
    STATE(io, vram_increment);
    STATE(io, OAMADDR);
    // (the hardware's 8 slots only: the extra ones of "Disable Sprite Limit" are not saved)
    state_bytes(io, "sprite_tiles_bit0", sprite_tiles_bit0, SPRITE_HW);
    state_bytes(io, "sprite_tiles_bit1", sprite_tiles_bit1, SPRITE_HW);
    state_bytes(io, "sprite_attrib_latches", sprite_attrib_latches, SPRITE_HW);
    state_bytes(io, "sprite_x_counters", sprite_x_counters, SPRITE_HW);
    state_bytes(io, "sprite_counting", sprite_counting, SPRITE_HW);
    STATE_SKIP(sprite_no_limit);
    STATE(io, oam_bus);
    STATE(io, oam2_addr);
    STATE(io, spr_in_range);
    STATE(io, spr_copy_done);
    STATE(io, spr0_added);
    STATE(io, spr0_visible);
    STATE(io, spr_overflow_bug);
    STATE(io, spr_prev_rendering);
    STATE(io, render_history);
    STATE(io, render_eff_history);
    STATE(io, ppu_bus_addr);
    STATE(io, bus_a12);
    STATE(io, a12_due);
    STATE(io, a12_count);
    STATE(io, a12_head_level);
    STATE(io, a12_sprite);
    STATE_SKIP(a12_src_sprite);       // (only set inside a sprite fetch)
    STATE_SKIP(a12_delivered_sprite); // (set before each hook call)
    STATE(io, ppudata_read_delay);
    STATE(io, ppudata_inc_pending);
    STATE(io, ppudata_collide);
    STATE(io, v_copy_delay);
    STATE(io, v_copy_value);
    STATE(io, last_2007_read_cycle);
    STATE_SKIP(sprite_live);   // derived from the shifters (below)
    STATE_SKIP(sprite_dots);   // only differences matter
    STATE_SKIP(sprite_synced); // all up to date after sprite_sync_all()
    if (state_loading(io)) {
        // (the extra slots of "Disable Sprite Limit" are not in the state: empty)
        memset(&sprite_tiles_bit0[SPRITE_HW], 0, SPRITE_SLOTS - SPRITE_HW);
        memset(&sprite_tiles_bit1[SPRITE_HW], 0, SPRITE_SLOTS - SPRITE_HW);
        memset(&sprite_counting[SPRITE_HW], 0, SPRITE_SLOTS - SPRITE_HW);
        sprite_extras_used = 0;
        for (unsigned n = 0; n < SPRITE_SLOTS; n++) {
            sprite_set_live(n);
            sprite_synced[n] = sprite_dots;
        }
    }
    STATE_SKIP(sprite_extras_used); // (see above)
    STATE_SKIP(ppu_thread);         // the coroutine (restarted by ppu_state_restart())
    STATE_SKIP(apu_log_active);     // --apu-log
    STATE_SKIP(s0_hit_line);        // debugger information (the last sprite 0 hit)
    STATE_SKIP(s0_hit_x);
    STATE_SKIP(s0_hit_frame);
    STATE_SKIP(line_scroll); // debugger information (the background viewer's scroll per line)
}
