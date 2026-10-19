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
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include "log.h"
#include "rp2a03.h"
#include "cpu.h"
#include "bits.h"
#include "ppu.h"
#include "mapper.h"
#include "zapper.h"
#include "audio.h"
#include "libco/libco.h"
#ifdef ONE_DEBUGGER
#include "debug_hooks.h"
#endif

#define FRAME_COUNTER_MODE_4_STEP (0)
#define FRAME_COUNTER_MODE_5_STEP (BIT_7)
#define FRAME_COUNTER_IRQ_INHIBIT (BIT_6)

// https://wiki.nesdev.com/w/index.php/APU_Length_Counter
static const uint8_t length_counter_load_table[32] = {10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
                                                      12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};

static const uint8_t pulse_duty_table[4][8] = {
    // read with duty_pos counting down
    {0, 0, 0, 0, 0, 0, 0, 1},
    {0, 0, 0, 0, 0, 0, 1, 1},
    {0, 0, 0, 0, 1, 1, 1, 1},
    {1, 1, 1, 1, 1, 1, 0, 0},
};
static const uint8_t triangle_sequence[32] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5,  4,  3,  2,  1,  0,
                                              0,  1,  2,  3,  4,  5,  6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static const uint16_t noise_period_table[16] = {4,   8,   16,  32,  64,  96,   128,  160,
                                                202, 254, 380, 508, 762, 1016, 2034, 4068};

// https://wiki.nesdev.com/w/index.php/APU
typedef struct {
    const char *name;
    uint8_t
        duty_envelope; // DDLC VVVV   Duty (D), envelope loop / length counter halt (L), constant volume (C), volume/envelope (V)
    uint8_t sweep_unit;            // EPPP NSSS   Sweep unit: enabled (E), period (P), negate (N), shift (S)
    uint8_t period_low;            // TTTT TTTT   Timer low (T)
    uint8_t reload_length_counter; // LLLL LTTT   Length counter load (L), timer high (T)
    uint8_t length_counter;
    const uint8_t length_counter_halt_mask; // bit that indicates Halt
    // Register writes reach the length counter after that cycle's frame-counter clock (Mesen ApuLengthCounter;
    // blargg_apu_2005 10.len_halt_timing / 11.len_reload_timing): a halt-flag change applies after the clock, and a
    // length load is ignored if the clock just decremented a non-zero counter.
    bool halt;              // halt flag in effect
    bool new_halt;          // written value, applied by length_counter_apply()
    uint8_t reload_value;   // pending length load (0 = none)
    uint8_t previous_value; // counter value at the time of the load write

    // Sound generation (names as in Mesen's APU state, which tools/mesen/apu_log.lua records)
    uint16_t timer;        // counts CPU cycles down to 0, then reloads from timer_period and clocks the channel
    uint16_t timer_period; // in CPU cycles: pulse 2 * real_period + 1, triangle real_period, noise table - 1
    uint16_t real_period;  // the 11-bit period from the registers (pulse, triangle)
    uint8_t output;        // the channel's current output level, 0-15
    // envelope (pulse, noise); its loop flag is the length counter's halt flag
    bool env_start;
    bool env_constant;
    uint8_t env_volume; // the register's volume / envelope period
    int8_t env_divider;
    uint8_t env_counter; // the decaying volume, 15 -> 0
    // pulse
    uint8_t duty;
    uint8_t duty_pos;
    bool sweep_enabled, sweep_negate, sweep_reload;
    uint8_t sweep_period; // register value + 1
    uint8_t sweep_shift;
    uint8_t sweep_divider;
    uint16_t sweep_target;
    bool ones_complement; // pulse 1 negates with one's complement (period - change - 1)
    // triangle
    bool linear_control, linear_reload_flag;
    uint8_t linear_reload, linear_counter;
    uint8_t sequence_pos;
    // noise
    uint16_t shift_register;
    bool mode_flag;
} apu_channel_t;

static uint32_t apu_clock;

extern uint8_t OAMADDR;
extern uint8_t obj_attrib_mem[0x100];
static uint8_t joy1_strobe;
static uint32_t joy1_latch; // 8 button bits; with a Four Score 24 (players 1 / 2, 3 / 4, the adapter's signature)
static uint8_t joy2_strobe;
static uint32_t joy2_latch;
// $4016 writes reach the controller port OUT pins only at the start of a "put" cycle (AccuracyCoin "Controller
// Strobing" 3/4; Mesen NesControlManager): a write on cycle c takes effect at cycle c+1 (c odd) or c+2 (c even). A
// newer write before then replaces it, so DEC $4016's 1-cycle strobe only happens when its first write is on an
// odd cycle.
static bool joy_write_pending = false;
static uint8_t joy_write_value;
static uint64_t joy_write_cycle;

// The Four Score's signature bits, as Mesen reads them (24 reads: player 1 / 2, player 3 / 4, signature)
#define FOUR_SCORE_SIGNATURE_1 0x08u
#define FOUR_SCORE_SIGNATURE_2 0x04u

static void joy_apply_strobe(uint8_t value)
{
    value &= BIT_0;
    if ((value == 0) && (joy1_strobe == 1)) {
        joy1_latch = input_joy1_read();
        joy2_latch = input_joy2_read(); // the strobe ($4016 bit 0) goes to both controller ports
        if (input_four_score()) {       // players 3 and 4 (no buttons), then the signature that tells the game
            joy1_latch |= FOUR_SCORE_SIGNATURE_1 << 16;
            joy2_latch |= FOUR_SCORE_SIGNATURE_2 << 16;
        }
    }
    joy1_strobe = value;
}

// Apply a pending $4016 write once its put cycle has been reached. Called before any controller access.
static void joy_update(void)
{
    if (joy_write_pending && (cpu_cycle >= joy_write_cycle)) {
        joy_write_pending = false;
        joy_apply_strobe(joy_write_value);
    }
}

static uint8_t reg_dmc_rate = 0;              // $4010
static uint16_t reg_dmc_sample_addr = 0xC000; // $4012
static uint16_t reg_dmc_sample_len = 1;       // $4013
static uint8_t reg_apu_status;                // $4015

static uint16_t dmc_timer = 428; // counts down to zero
static const uint16_t DMC_NTSC_RATE_CYCLES[16] = {428, 380, 340, 320, 286, 254, 226, 214,
                                                  190, 160, 142, 128, 106, 84,  72,  54};
static uint8_t dmc_output = 0;
static uint8_t dmc_silence_flag = 0;
static uint8_t dmc_shift_reg = 0;
static uint16_t dmc_address = 0;
static uint8_t dmc_bits_remaining = 8;
static uint16_t dmc_bytes_remaining = 0;
static uint8_t dmc_irq = 0; // only BIT_7 is used in this variable
static uint8_t dmc_buffer;
static bool dmc_buffer_full;
static bool oam_dma_pending; // a $4014 write is waiting for the CPU's next read cycle
static uint8_t oam_dma_page;
static bool dma_seen = false; // debuggers: the last OAM DMA (rp2a03_debug_oam_dma())
static uint8_t dma_last_page;
static uint32_t dma_last_frame;
static bool dmc_dma_request; // the CPU should do a DMC fetch
// Mesen-matched delays, in APU ticks. apu_step() runs at the end of every CPU cycle, including the cycle of the
// $4015 write itself, hence the +1 against Mesen's 2/3 (start, stop) and 0 (refill). cpu_cycle in rp2a03_write()
// is the write cycle's number minus one.
static uint8_t dmc_start_delay;   // $4015 enable -> first fetch request
static uint8_t dmc_disable_delay; // $4015 disable -> bytes remaining cleared
static uint8_t dmc_refill_delay;  // output unit emptied the buffer -> fetch request


static uint8_t reg_frame_counter; // $4017
static uint32_t frame_counter;    // $4017
static int frame_counter_reset;

static apu_channel_t pulse_channel_1 = {.name = "pulse_1", .length_counter_halt_mask = BIT_5, .ones_complement = true};
static apu_channel_t pulse_channel_2 = {.name = "pulse_2", .length_counter_halt_mask = BIT_5};
static apu_channel_t triangle_channel = {.name = "triangle", .length_counter_halt_mask = BIT_7};
static apu_channel_t noise_channel = {.name = "noise", .length_counter_halt_mask = BIT_5};

static unsigned int frame_interrupt_flag = 0;
// A $4015 read releases the frame IRQ line at once, but the flag itself is only cleared on the next put -> get
// transition (AccuracyCoin "Frame Counter IRQ" tests 6/7, used by its get/put detection: SLO $4015,X reads $4015 on
// two consecutive cycles). A read on cycle c clears it at cycle c+1 (c even) or c+2 (c odd) -- Mesen 2.2
// ApuFrameCounter::GetIrqFlag(). 0 = no clear pending.
static uint64_t frame_flag_clear_cycle = 0;

// Apply a pending clear of the frame interrupt flag once its cycle has been reached.
static inline void frame_flag_update(void)
{
    if (frame_flag_clear_cycle && (cpu_cycle >= frame_flag_clear_cycle)) {
        frame_interrupt_flag = 0;
        frame_flag_clear_cycle = 0;
    }
}

static void apu_step(void);

// --- mixer --------------------------------------------------------------------------------------------------------
// The NES mixes its channels through resistor networks, not linearly (nesdev wiki "APU Mixer"):
//   pulse_out = 95.88 / (8128 / (pulse1 + pulse2) + 100)
//   tnd_out   = 159.79 / (1 / (triangle / 8227 + noise / 12241 + dmc / 22638) + 100)
// (the exact form: the 24329 / (3 * triangle + 2 * noise + dmc) lookup-table version is off by up to 1/4 of a DMC
// step, enough for blargg's apu_mixer/dmc.nes to leave an audible tone). Their sum (0.0 .. about 1.0) is averaged
// over the CPU cycles of each output sample, which is then filtered and played by audio.c.
static bool audio_on;      // audio_active() at power-on/reset: no listener = no samples
static double audio_phase; // output samples owed, fraction
static double audio_step;  // output samples per CPU cycle
static double audio_sum;   // mixer levels since the last sample
static int audio_count;
static double expansion_level;        // a mapper's audio (rp2a03_set_expansion())
static uint32_t mix_key = UINT32_MAX; // the channel outputs mix_level was computed for
static double mix_level;

static void mixer_init(void)
{
    audio_on = audio_active();
    audio_phase = 0.0;
    audio_step = audio_samples_per_cycle();
    audio_sum = 0.0;
    audio_count = 0;
    mix_key = UINT32_MAX;
}

static double mixer_level(unsigned pulse1, unsigned pulse2, unsigned triangle, unsigned noise, unsigned dmc)
{
    double pulse = 0.0;
    if (pulse1 + pulse2 != 0) {
        pulse = 95.88 / (8128.0 / (pulse1 + pulse2) + 100.0);
    }
    double tnd = 0.0;
    if (triangle + noise + dmc != 0) {
        tnd = 159.79 / (1.0 / (triangle / 8227.0 + noise / 12241.0 + dmc / 22638.0) + 100.0);
    }
    return pulse + tnd;
}

static inline void mixer_run(void)
{
    // the outputs change at most every few cycles: recompute the level only then
    uint32_t key = (uint32_t)pulse_channel_1.output | ((uint32_t)pulse_channel_2.output << 4) |
                   ((uint32_t)triangle_channel.output << 8) | ((uint32_t)noise_channel.output << 12) |
                   ((uint32_t)dmc_output << 16);
    if (key != mix_key) {
        mix_key = key;
        mix_level = mixer_level(pulse_channel_1.output, pulse_channel_2.output, triangle_channel.output,
                                noise_channel.output, dmc_output);
    }
    audio_sum += mix_level + expansion_level;
    audio_count++;
    audio_phase += audio_step;
    if (audio_phase >= 1.0) {
        audio_phase -= 1.0;
        audio_sample((float)(audio_sum / audio_count));
        audio_sum = 0.0;
        audio_count = 0;
        audio_step = audio_samples_per_cycle(); // rate control may have nudged it
    }
}

void rp2a03_set_expansion(double level)
{
    expansion_level = level;
}

void rp2a03_init()
{
    mixer_init();
}

void rp2a03_cleanup() {}

#ifndef APU_RESET_4017_DELAY
#define APU_RESET_4017_DELAY 3
#endif
void rp2a03_power_on()
{
    dma_seen = false;
    joy1_strobe = joy1_latch = 0;
    joy2_strobe = joy2_latch = 0;
    joy_write_pending = false;
    reg_dmc_sample_addr = 0xC000;
    reg_dmc_sample_len = 1;
    apu_clock = 0;
    reg_frame_counter = 0;
    frame_counter = 0;
    // "After reset or power-up, APU acts as if $4017 were written with $00 from 9 to 12 clocks before first
    // instruction begins" (nesdev; apu_reset/4017_timing): the frame counter restarts 3 cycles in (Mesen Reset()).
    frame_counter_reset = APU_RESET_4017_DELAY;
    reg_apu_status = 0;

    dmc_irq = 0;
    reg_dmc_rate = 0;
    dmc_timer = DMC_NTSC_RATE_CYCLES[reg_dmc_rate & 0x0F];
    dmc_bits_remaining = 8;

    // the sound generators start silent; the noise shift register starts at 1 (Mesen)
    apu_channel_t *channels[4] = {&pulse_channel_1, &pulse_channel_2, &triangle_channel, &noise_channel};
    for (int i = 0; i < 4; i++) {
        apu_channel_t *c = channels[i];
        c->timer = c->timer_period = c->real_period = 0;
        c->output = 0;
        c->env_start = c->env_constant = false;
        c->env_volume = c->env_counter = 0;
        c->env_divider = 0;
        c->duty = c->duty_pos = 0;
        c->sweep_enabled = c->sweep_negate = c->sweep_reload = false;
        c->sweep_period = c->sweep_shift = c->sweep_divider = 0;
        c->sweep_target = 0;
        c->linear_control = c->linear_reload_flag = false;
        c->linear_reload = c->linear_counter = 0;
        c->sequence_pos = 0;
    }
    noise_channel.shift_register = 1;
    noise_channel.mode_flag = false;
    noise_channel.timer_period = (uint16_t)(noise_period_table[0] - 1);
}

// Soft reset (RESET button): APU restarts like power-on (fresh coroutine, frame counter restarted), channels are
// silenced ($4015 = 0) and the frame interrupt is cleared.
void rp2a03_reset(void)
{
    rp2a03_init(); // recreate the APU coroutine
    apu_clock = 0;
    frame_counter = 0;
    frame_counter_reset = APU_RESET_4017_DELAY;      // the last $4017 mode is rewritten (see rp2a03_power_on)...
    reg_frame_counter &= ~FRAME_COUNTER_IRQ_INHIBIT; // ...but the IRQ inhibit flag is cleared (apu_reset/4017_written)
    reg_apu_status = 0;
    pulse_channel_1.length_counter = 0;
    pulse_channel_2.length_counter = 0;
    triangle_channel.length_counter = 0;
    noise_channel.length_counter = 0;
    // pending length loads / halt flags are dropped too (Mesen: the triangle's halt flag is unaffected by reset)
    pulse_channel_1.reload_value = pulse_channel_2.reload_value = noise_channel.reload_value = 0;
    triangle_channel.reload_value = 0;
    pulse_channel_1.halt = pulse_channel_1.new_halt = false;
    pulse_channel_2.halt = pulse_channel_2.new_halt = false;
    noise_channel.halt = noise_channel.new_halt = false;
    frame_interrupt_flag = 0;
    frame_flag_clear_cycle = 0;
    // DMC: $4010 (rate, loop, IRQ enable) is cleared and the output unit restarts; $4012/$4013 are kept (Mesen
    // DeltaModulationChannel::Reset; apu_reset/works_immediately's DMC fetch timing after reset).
    reg_dmc_rate = 0;
    dmc_timer = DMC_NTSC_RATE_CYCLES[0];
    dmc_bits_remaining = 8;
    dmc_buffer_full = false;
    dmc_silence_flag = 1;
    dmc_shift_reg = 0;
    dmc_output = 0;
    dmc_irq = 0;
    dmc_dma_request = false;
    oam_dma_pending = false;
    dmc_start_delay = dmc_disable_delay = dmc_refill_delay = 0;
    dmc_bytes_remaining = 0;
    cpu_irq_request(false, BIT_0);
    cpu_irq_request(false, BIT_1);
}

uint8_t rp2a03_read(uint16_t address)
{
    uint8_t value = 0xA5;

    switch (address) {
    case 0x4015:
        value = 0;
        if (pulse_channel_1.length_counter != 0) {
            value |= BIT_0;
        }
        if (pulse_channel_2.length_counter != 0) {
            value |= BIT_1;
        }
        if (triangle_channel.length_counter != 0) {
            value |= BIT_2;
        }
        if (noise_channel.length_counter != 0) {
            value |= BIT_3;
        }
        if (dmc_bytes_remaining > 0) {
            value |= BIT_4;
        }
        frame_flag_update();
        if (frame_interrupt_flag) {
            value |= BIT_6; // frame interrupt flag is returned in bit 6, then cleared (see frame_flag_clear_cycle)
        }
        value |= dmc_irq;                    // sets BIT_7
        value |= (cpu_internal_bus & BIT_5); // bit 5 is not driven: the internal data bus shows through
        if (frame_interrupt_flag) {
            if (frame_flag_clear_cycle == 0) {
                frame_flag_clear_cycle = cpu_cycle + ((cpu_cycle & 1) ? 2 : 1);
            }
            cpu_irq_request(false, BIT_0); // the IRQ line is released immediately
        }
        break;
    case 0x4016:
        // https://wiki.nesdev.com/w/index.php/Standard_controller
        joy_update();
        input_note_poll();
        value = (joy1_latch & BIT_0) | (cpu_open_bus & 0xE0); // bits 5-7 are open bus (usually $40)
        joy1_latch = (joy1_latch >> 1) | (input_four_score() ? 0x800000u : 0x80u); // 1s after the 8 (24) bits read
        break;
    case 0x4017:
        joy_update();
        input_note_poll();
        value = (joy2_latch & BIT_0) | (cpu_open_bus & 0xE0); // bits 5-7 are open bus (usually $40)
        joy2_latch = (joy2_latch >> 1) | (input_four_score() ? 0x800000u : 0x80u);
        if (input_zapper_present()) {
            value =
                (cpu_open_bus & 0xE0) | zapper_read(); // a Zapper instead of the controller: light sense and trigger
        }
        break;

    default:
        value = cpu_open_bus; // write-only register: nothing drives the bus
        break;
    }
    return value;
}

// Side-effect-free peek for the trace (matches Mesen): $4015 returns APU status without clearing the frame IRQ;
// every other register in $4000-$4017 shows open bus, which for an absolute access is the address high byte ($40).
uint8_t rp2a03_debug_read(uint16_t address)
{
    uint8_t value = 0x40;
    if ((address == 0x4016) || (address == 0x4017)) {
        value = 0x00; // Mesen peeks the controller ports as $00
    } else if (address == 0x4015) {
        value = 0;
        if (pulse_channel_1.length_counter != 0)
            value |= BIT_0;
        if (pulse_channel_2.length_counter != 0)
            value |= BIT_1;
        if (triangle_channel.length_counter != 0)
            value |= BIT_2;
        if (noise_channel.length_counter != 0)
            value |= BIT_3;
        // (a disable whose delay ends at the end of this cycle already shows as clear, as in Mesen's peek)
        if ((dmc_bytes_remaining > 0) && (dmc_disable_delay != 1))
            value |= BIT_4;
        if (frame_interrupt_flag && !(frame_flag_clear_cycle && (cpu_cycle >= frame_flag_clear_cycle))) {
            value |= BIT_6; // (a pending clear is not applied by a peek)
        }
        value |= dmc_irq; // sets BIT_7
    }
    return value;
}

// --- sound channels (https://www.nesdev.org/wiki/APU; the details follow Mesen 2.2) ---------------------------------

static uint8_t envelope_volume(const apu_channel_t *c)
{
    if (c->length_counter == 0) {
        return 0;
    }
    return c->env_constant ? c->env_volume : c->env_counter;
}

static void envelope_write(apu_channel_t *c, uint8_t value)
{
    c->env_constant = (value & BIT_4) != 0;
    c->env_volume = value & 0x0F;
}

static void envelope_clock(apu_channel_t *c)
{
    if (c->env_start) {
        c->env_start = false;
        c->env_counter = 15;
        c->env_divider = (int8_t)c->env_volume;
    } else if (--c->env_divider < 0) {
        c->env_divider = (int8_t)c->env_volume;
        if (c->env_counter > 0) {
            c->env_counter--;
        } else if (c->halt) { // the loop flag
            c->env_counter = 15;
        }
    }
}

static void pulse_update_target(apu_channel_t *c)
{
    uint16_t change = c->real_period >> c->sweep_shift;
    if (c->sweep_negate) {
        c->sweep_target = (uint16_t)(c->real_period - change - (c->ones_complement ? 1 : 0));
    } else {
        c->sweep_target = (uint16_t)(c->real_period + change);
    }
}

static void pulse_set_period(apu_channel_t *c, uint16_t period)
{
    c->real_period = period;
    c->timer_period = (uint16_t)(period * 2 + 1);
    pulse_update_target(c);
}

// A period under 8, or a sweep target over $7FF (even with the sweep disabled), silences the channel
static bool pulse_muted(const apu_channel_t *c)
{
    return (c->real_period < 8) || (!c->sweep_negate && (c->sweep_target > 0x7FF));
}

static void channel_state(const apu_channel_t *c, rp2a03_channel_state *out)
{
    out->period = c->real_period;
    out->volume = c->env_constant ? c->env_volume : c->env_counter;
    out->duty = c->duty;
    out->length = c->length_counter;
    out->linear = c->linear_counter;
    out->muted = false;
    out->short_mode = c->mode_flag;
}

void rp2a03_debug_state_get(rp2a03_debug_state *out)
{
    channel_state(&pulse_channel_1, &out->pulse[0]);
    channel_state(&pulse_channel_2, &out->pulse[1]);
    channel_state(&triangle_channel, &out->triangle);
    channel_state(&noise_channel, &out->noise);
    out->pulse[0].muted = pulse_muted(&pulse_channel_1);
    out->pulse[1].muted = pulse_muted(&pulse_channel_2);
    out->noise.period = 0;
    for (uint16_t i = 0; i < 16; i++) {
        if (noise_period_table[i] - 1 == noise_channel.timer_period) {
            out->noise.period = i;
        }
    }
    out->dmc_rate = reg_dmc_rate & 0x0F;
    out->dmc_loop = (reg_dmc_rate & BIT_6) != 0;
    out->dmc_address = reg_dmc_sample_addr;
    out->dmc_length = reg_dmc_sample_len;
    out->dmc_remaining = dmc_bytes_remaining;
    out->dmc_output = dmc_output;
}

static void pulse_update_output(apu_channel_t *c)
{
    c->output = pulse_muted(c) ? 0 : (uint8_t)(pulse_duty_table[c->duty][c->duty_pos] * envelope_volume(c));
}

static void pulse_write(apu_channel_t *c, int reg, uint8_t value)
{
    switch (reg) {
    case 0:
        envelope_write(c, value);
        c->duty = (value >> 6) & 3;
        break;
    case 1:
        c->sweep_enabled = (value & BIT_7) != 0;
        c->sweep_negate = (value & BIT_3) != 0;
        c->sweep_period = (uint8_t)(((value >> 4) & 7) + 1);
        c->sweep_shift = value & 7;
        pulse_update_target(c);
        c->sweep_reload = true;
        break;
    case 2: pulse_set_period(c, (uint16_t)((c->real_period & 0x0700) | value)); break;
    default:
        pulse_set_period(c, (uint16_t)((c->real_period & 0x00FF) | ((value & 7) << 8)));
        c->duty_pos = 0;
        c->env_start = true;
        break;
    }
    pulse_update_output(c);
}

static void sweep_clock(apu_channel_t *c)
{
    if (--c->sweep_divider == 0) {
        if ((c->sweep_shift > 0) && c->sweep_enabled && (c->real_period >= 8) && (c->sweep_target <= 0x7FF)) {
            pulse_set_period(c, c->sweep_target);
        }
        c->sweep_divider = c->sweep_period;
    }
    if (c->sweep_reload) {
        c->sweep_divider = c->sweep_period;
        c->sweep_reload = false;
    }
}

static void linear_counter_clock(apu_channel_t *c)
{
    if (c->linear_reload_flag) {
        c->linear_counter = c->linear_reload;
    } else if (c->linear_counter > 0) {
        c->linear_counter--;
    }
    if (!c->linear_control) {
        c->linear_reload_flag = false;
    }
}

// One CPU cycle of the four tone channels (the DMC runs in apu_step())
static inline void channels_run(void)
{
    apu_channel_t *pulses[2] = {&pulse_channel_1, &pulse_channel_2};
    for (int i = 0; i < 2; i++) {
        apu_channel_t *c = pulses[i];
        if (c->timer == 0) {
            c->timer = c->timer_period;
            c->duty_pos = (c->duty_pos - 1) & 7;
            pulse_update_output(c);
        } else {
            c->timer--;
        }
    }
    apu_channel_t *t = &triangle_channel;
    if (t->timer == 0) {
        t->timer = t->timer_period;
        if ((t->length_counter != 0) && (t->linear_counter > 0)) {
            t->sequence_pos = (t->sequence_pos + 1) & 0x1F;
            t->output = triangle_sequence[t->sequence_pos];
        }
    } else {
        t->timer--;
    }
    apu_channel_t *n = &noise_channel;
    if (n->timer == 0) {
        n->timer = n->timer_period;
        uint16_t feedback = (n->shift_register ^ (n->shift_register >> (n->mode_flag ? 6 : 1))) & 1;
        n->shift_register = (uint16_t)((n->shift_register >> 1) | (feedback << 14));
        n->output = (n->shift_register & 1) ? 0 : envelope_volume(n);
    } else {
        n->timer--;
    }
}

// http://wiki.nesdev.com/w/index.php/2A03
void rp2a03_write(uint16_t address, uint8_t value)
{
    switch (address) {
    case 0x4000:
        pulse_channel_1.duty_envelope = value;
        pulse_channel_1.new_halt = (value & pulse_channel_1.length_counter_halt_mask) != 0;
        pulse_write(&pulse_channel_1, 0, value);
        break;
    case 0x4001:
        pulse_channel_1.sweep_unit = value;
        pulse_write(&pulse_channel_1, 1, value);
        break;
    case 0x4002:
        pulse_channel_1.period_low = value;
        pulse_write(&pulse_channel_1, 2, value);
        break;
    case 0x4003: {
        if (reg_apu_status & BIT_0) {
            uint8_t length_counter_index = (value >> 3);
            pulse_channel_1.reload_value = length_counter_load_table[length_counter_index];
            pulse_channel_1.previous_value = pulse_channel_1.length_counter;
        }
        pulse_channel_1.reload_length_counter = value;
        pulse_write(&pulse_channel_1, 3, value);
        break;
    }
    case 0x4004:
        pulse_channel_2.duty_envelope = value;
        pulse_channel_2.new_halt = (value & pulse_channel_2.length_counter_halt_mask) != 0;
        pulse_write(&pulse_channel_2, 0, value);
        break;
    case 0x4005:
        pulse_channel_2.sweep_unit = value;
        pulse_write(&pulse_channel_2, 1, value);
        break;
    case 0x4006:
        pulse_channel_2.period_low = value;
        pulse_write(&pulse_channel_2, 2, value);
        break;
    case 0x4007: {
        if (reg_apu_status & BIT_1) {
            uint8_t length_counter_index = (value >> 3);
            pulse_channel_2.reload_value = length_counter_load_table[length_counter_index];
            pulse_channel_2.previous_value = pulse_channel_2.length_counter;
        }
        pulse_channel_2.reload_length_counter = value;
        pulse_write(&pulse_channel_2, 3, value);
        break;
    }

    case 0x4008:
        triangle_channel.duty_envelope = value; // todo: this isn't really "duty envelope"
        triangle_channel.new_halt = (value & triangle_channel.length_counter_halt_mask) != 0;
        triangle_channel.linear_control = (value & BIT_7) != 0;
        triangle_channel.linear_reload = value & 0x7F;
        break;
    case 0x400A:
        triangle_channel.period_low = value;
        triangle_channel.real_period = (uint16_t)((triangle_channel.real_period & 0x0700) | value);
        triangle_channel.timer_period = triangle_channel.real_period;
        break;
    case 0x400B: {
        if (reg_apu_status & BIT_2) {
            uint8_t length_counter_index = (value >> 3);
            triangle_channel.reload_value = length_counter_load_table[length_counter_index];
            triangle_channel.previous_value = triangle_channel.length_counter;
        }
        triangle_channel.reload_length_counter = value;
        triangle_channel.real_period = (uint16_t)((triangle_channel.real_period & 0x00FF) | ((value & 7) << 8));
        triangle_channel.timer_period = triangle_channel.real_period;
        triangle_channel.linear_reload_flag = true;
        break;
    }

    case 0x400C:
        noise_channel.duty_envelope = value;
        noise_channel.new_halt = (value & noise_channel.length_counter_halt_mask) != 0;
        envelope_write(&noise_channel, value);
        break;
    case 0x400E:
        noise_channel.period_low = value; // (mode and period index)
        noise_channel.timer_period = (uint16_t)(noise_period_table[value & 0x0F] - 1);
        noise_channel.mode_flag = (value & BIT_7) != 0;
        break;
    case 0x400F: {
        if (reg_apu_status & BIT_3) {
            uint8_t length_counter_index = (value >> 3);
            noise_channel.reload_value = length_counter_load_table[length_counter_index];
            noise_channel.previous_value = noise_channel.length_counter;
        }
        noise_channel.reload_length_counter = value;
        noise_channel.env_start = true;
        break;
    }
    case 0x4010: // DMC Rate
        reg_dmc_rate = value;
        if (!(reg_dmc_rate & BIT_7)) {
            dmc_irq = 0;
            cpu_irq_request(false, BIT_1);
        }
        break;
    case 0x4011: // DMC Direct Load
        dmc_output = value & 0x7F;
        // todo: produce audio
        break;
    case 0x4012: // DMC Sample Address
        reg_dmc_sample_addr = 0xC000 + (value << 6);
        break;
    case 0x4013: // DMC Sample Length
        reg_dmc_sample_len = (value << 4) + 1;
        break;

    case 0x4014:
        // The write only requests the DMA; the CPU halts on its next read cycle (DMA never starts on a write cycle).
        // A second write before then (INC $4014) just changes the page: one DMA, with the last value (AccuracyCoin
        // "INC $4014").
        oam_dma_pending = true;
        oam_dma_page = value;
        LOG_TRACE(DMA, "OAM DMA requested from page %02X", value);
        break;

    case 0x4015:
        reg_apu_status = (value & 0x1F); // Bits: ---D NT21
        dmc_irq = 0;
        cpu_irq_request(false, BIT_1);

        if (!(reg_apu_status & BIT_0)) {
            pulse_channel_1.length_counter = 0;
        }
        if (!(reg_apu_status & BIT_1)) {
            pulse_channel_2.length_counter = 0;
        }
        if (!(reg_apu_status & BIT_2)) {
            triangle_channel.length_counter = 0;
        }
        if (!(reg_apu_status & BIT_3)) {
            noise_channel.length_counter = 0;
        }
        if (reg_apu_status & BIT_4) {
            if (dmc_bytes_remaining == 0) {
                dmc_address = reg_dmc_sample_addr;
                dmc_bytes_remaining = reg_dmc_sample_len;
                dmc_start_delay = ((cpu_cycle & 1) ? 3 : 2) + 1;
            }
        } else if (dmc_disable_delay == 0) {
            dmc_disable_delay = ((cpu_cycle & 1) ? 3 : 2) + 1;
        }
        break;

    case 0x4016:
        joy_update();
        joy_write_pending = true;
        joy_write_value = value;
        joy_write_cycle = cpu_cycle + ((cpu_cycle & 1) ? 1 : 2);
        break;
    case 0x4017:
        // writes to 4017 have nothing to do with controller port 2, it is APU frame counter
        reg_frame_counter = (value & 0xC0);
        if (value & BIT_6) {
            // clear frame_interrupt_flag
            if (frame_interrupt_flag) {
                frame_interrupt_flag = 0;
                cpu_irq_request(false, BIT_0);
            }
            frame_flag_clear_cycle = 0;
        }
        // The sequencer resets 3 or 4 CPU cycles after the write, depending on the CPU cycle's parity (nesdev APU Frame
        // Counter). cpu_cycle is the number of the write cycle here. Measured against Mesen (instr_timing/1): a write on
        // an odd cycle takes one cycle longer than a write on an even cycle.
        if (cpu_cycle & 1) {
            frame_counter_reset = 4;
        } else {
            frame_counter_reset = 3;
        }
        LOG_DEBUG(APU, "$4017 = %02X: %d-step sequence, IRQ %s", reg_frame_counter,
                  (reg_frame_counter & FRAME_COUNTER_MODE_5_STEP) ? 5 : 4,
                  (reg_frame_counter & FRAME_COUNTER_IRQ_INHIBIT) ? "inhibited" : "enabled");
        break;
    default: LOG_DEBUG(APU, "write to unused register %04X = %02X", address, value); break;
    }
}

static void length_counter_apply(apu_channel_t *p_channel)
{
    if (p_channel->reload_value) {
        if (p_channel->length_counter == p_channel->previous_value) {
            p_channel->length_counter = p_channel->reload_value;
        }
        p_channel->reload_value = 0;
    }
    p_channel->halt = p_channel->new_halt;
}

static void length_counter_clock(apu_channel_t *p_channel)
{
    assert(p_channel != NULL);

    // only decrement if length isn't already 0 and if Halt isn't set
    if ((p_channel->length_counter != 0) && !p_channel->halt) {
        p_channel->length_counter = p_channel->length_counter - 1;
    }
}

static void quarter_frame_clock(void)
{
    envelope_clock(&pulse_channel_1);
    envelope_clock(&pulse_channel_2);
    envelope_clock(&noise_channel);
    linear_counter_clock(&triangle_channel);
}

static void half_frame_clock(void)
{
    length_counter_clock(&pulse_channel_1);
    length_counter_clock(&pulse_channel_2);
    length_counter_clock(&triangle_channel);
    length_counter_clock(&noise_channel);
    sweep_clock(&pulse_channel_1);
    sweep_clock(&pulse_channel_2);
}

// Mode 0 sets the frame IRQ flag on each of its last three steps (29828, 29829, 29830), so a $4015 read that clears
// it on 29828 or 29829 sees it set again (nesdev APU Frame Counter; Mesen on instr_timing/2-branch_timing).
static void frame_irq_step(int step) /* step: 0,1,2 = cycles 29828, 29829, 29830 */
{
    if (!(reg_frame_counter & FRAME_COUNTER_IRQ_INHIBIT)) {
        frame_interrupt_flag = 1;
        frame_flag_clear_cycle = 0;
        cpu_irq_request(true, BIT_0);
    }
#ifdef MESEN_COMPATIBILITY
    else if (step == 0) {
        // Mesen shows the frame interrupt flag in $4015 bit 6 at the end of each 4-step frame even while IRQs are
        // inhibited, but only briefly: set on the first of the three final steps (29828) and cleared again on the
        // last (29830). Measured on instr_timing: LDA $4015 -> $41 exactly 59661 cycles after writing $40 to $4017,
        // $00 a few cycles later. The nesdev wiki says inhibit prevents the flag, so this is Mesen-only behaviour:
        // status bit only, never the IRQ line.
        frame_interrupt_flag = 1;
        frame_flag_clear_cycle = 0;
    } else if (step == 2) {
        frame_interrupt_flag = 0;
        frame_flag_clear_cycle = 0;
    }
#endif
}

bool rp2a03_oam_dma_pending(void)
{
    return oam_dma_pending;
}

// OAM DMA, run when the CPU halts on a read of halt_address (Mesen 2.2 ProcessPendingDma). The halt cycle and every
// alignment / DMC halt / DMC dummy cycle re-read that address, except that a controller port only sees the first of
// several back-to-back reads. If that address is in $4000-$401F the DMA reads also reach the internal registers
// (cpu_dma_read()). Reads happen on "get" cycles (odd cpu_cycle at the access, as for the DMC read), $2004 writes
// on "put" cycles. A DMC fetch requested meanwhile uses sprite cycles as its halt and dummy cycles, then takes the
// next get cycle for itself: normally 2 extra cycles (the DMC read + re-alignment of the sprite reads).
bool rp2a03_debug_oam_dma(uint8_t *page, uint32_t *frame)
{
    *page = dma_last_page;
    *frame = dma_last_frame;
    return dma_seen;
}

void rp2a03_oam_dma(uint16_t halt_address)
{
    int i;
    uint16_t dma_address = (oam_dma_page << 8);
    oam_dma_pending = false;
    dma_seen = true;
    dma_last_page = oam_dma_page;
    dma_last_frame = ppu_debug_get_frame_num();

    bool internal_reads = ((halt_address & 0xFFE0) == 0x4000);
    bool skip_dummy_reads = (halt_address == 0x4016) || (halt_address == 0x4017);
    cpu_bus_read(halt_address); // halt cycle: https://forums.nesdev.com/viewtopic.php?f=3&t=14120

    i = 0;
    bool write_pending = false;
    bool dmc_active = false;
    int dmc_wait = 0; // halt + dummy cycles the DMC fetch still has to wait
    uint8_t data = 0;
    uint16_t prev_read = halt_address;
    while ((i < 256) || write_pending || dmc_active || rp2a03_dmc_dma_pending()) {
        if (!dmc_active && rp2a03_dmc_dma_pending()) {
            dmc_active = true;
            dmc_wait = 2;
        }
        bool get_cycle = ((cpu_cycle & 1) == 1);
        if (get_cycle && dmc_active && (dmc_wait == 0)) {
            rp2a03_dmc_dma_complete(cpu_dma_read(rp2a03_dmc_dma_address(), &prev_read, internal_reads)); // DMC read
            cpu_add_cycles(CPU_ONE_CYCLE);
            dmc_active = false;
            continue;
        }
        if (dmc_active && (dmc_wait > 0)) {
            dmc_wait--; // this cycle counts as the DMC's halt / dummy cycle
        }
        if (get_cycle && (i < 256) && !write_pending) {
            data = cpu_dma_read(dma_address + i, &prev_read, internal_reads); // sprite read
            cpu_add_cycles(CPU_ONE_CYCLE);
            write_pending = true;
            i++;
        } else if (!get_cycle && write_pending) {
            if ((OAMADDR & 0x03) == 0x02) {
                obj_attrib_mem[OAMADDR] = data & 0xE3; // byte2 has some bits missing
            } else {
                obj_attrib_mem[OAMADDR] = data;
            }
            OAMADDR++;
            ppu_set_open_bus(data);                 // the DMA's write to $2004 drives the PPU data bus...
            cpu_open_bus = cpu_internal_bus = data; // ...and the CPU data bus, like any write
            cpu_add_cycles(CPU_ONE_CYCLE);          // sprite write
            write_pending = false;
        } else if (skip_dummy_reads) {
            cpu_add_cycles(CPU_ONE_CYCLE); // alignment / DMC halt or dummy cycle
        } else {
            cpu_bus_read(halt_address); // alignment / DMC halt or dummy cycle
        }
    }
}

bool rp2a03_dmc_dma_pending(void)
{
    return dmc_dma_request;
}
uint16_t rp2a03_dmc_dma_address(void)
{
    return dmc_address;
}

// The CPU calls this with the byte it fetched.
void rp2a03_dmc_dma_complete(uint8_t value)
{
    LOG_TRACE(DMA, "DMC DMA read %02X (%u bytes left)", value, dmc_bytes_remaining);
#ifdef ONE_DEBUGGER
    if (dbg_dmc_hook) { // (the code/data log: a sample byte of the ROM)
        dbg_dmc_hook(dmc_address);
    }
#endif
    dmc_dma_request = false;
    if (dmc_bytes_remaining == 0) {
        return; // the DMC was disabled while this DMA ran: the byte is dropped (Mesen SetDmcReadBuffer)
    }
    dmc_buffer = value;
    dmc_buffer_full = true;
    dmc_address = (dmc_address == 0xFFFF) ? 0x8000 : dmc_address + 1;
    if (--dmc_bytes_remaining == 0) {
        if (reg_dmc_rate & BIT_6) { // loop: restart the sample
            dmc_address = reg_dmc_sample_addr;
            dmc_bytes_remaining = reg_dmc_sample_len;
        } else if (reg_dmc_rate & BIT_7) { // IRQ enabled
            dmc_irq = BIT_7;
            cpu_irq_request(true, BIT_1);
        }
    }
    // Implicit abort (AccuracyCoin "Implicit DMA Abort"; Mesen SetDmcReadBuffer): a 1-byte non-looping sample whose
    // fetch completes just before the output unit's bit counter reloads starts another fetch for the same sample,
    // which is aborted again almost at once -- the CPU sees a 1-cycle DMA.
    if ((reg_dmc_sample_len == 1) && !(reg_dmc_rate & BIT_6) && (dmc_bits_remaining == 1) && (dmc_timer <= 2)) {
        dmc_shift_reg = dmc_buffer; // buffer stays full: the reload empties it and requests a fetch
        dmc_address = reg_dmc_sample_addr;
        dmc_bytes_remaining = reg_dmc_sample_len;
        dmc_disable_delay = 3; // aborts the new fetch during its halt cycle
    }
}


// One CPU cycle of the APU: called at the end of every CPU cycle (cpu.c)
static void apu_step(void)
{
    // frame happens every 240Hz (21.4 Mhz / 89490)

    if (reg_frame_counter & FRAME_COUNTER_MODE_5_STEP) {
        // handle "mode 1" (5 step sequence)
        if (apu_clock == 7457) {
            quarter_frame_clock();
        } else if (apu_clock == 14913) {
            quarter_frame_clock();
            half_frame_clock();
        } else if (apu_clock == 22371) {
            quarter_frame_clock();
        } else if (apu_clock == 37281) {
            quarter_frame_clock();
            half_frame_clock();
        } else if (apu_clock == 37282) {
            apu_clock = 0;
        }
    } else {
        // handle "mode 0" (4 step sequence)
        if (apu_clock == 7457) {
            quarter_frame_clock();
        } else if (apu_clock == 14913) {
            quarter_frame_clock();
            half_frame_clock();
        } else if (apu_clock == 22371) {
            quarter_frame_clock();
        } else if (apu_clock == 29828) {
            frame_irq_step(0);
        } else if (apu_clock == 29829) {
            frame_irq_step(1);
            quarter_frame_clock();
            half_frame_clock();
        } else if (apu_clock == 29830) {
            frame_irq_step(2);
            apu_clock = 0;
        }
    }

    if (0 != frame_counter_reset) {
        frame_counter_reset--;
        if (0 == frame_counter_reset) {
            frame_counter = 0;
            apu_clock = 0;
            if (reg_frame_counter & BIT_7) {
                /* "Writing to $4017 with bit 7 set ($80) will immediately clock all of its
                   controlled units at the beginning of the 5-step sequence; with bit 7 clear,
                   only the sequence is reset without clocking any of its units." */
                quarter_frame_clock();
                half_frame_clock();
            }
        }
    }

    if (dmc_disable_delay && (--dmc_disable_delay == 0)) {
        dmc_bytes_remaining = 0;
        dmc_dma_request = false; // a fetch that hasn't started yet is cancelled
    }
    if (dmc_start_delay && (--dmc_start_delay == 0)) {
        dmc_dma_request = (!dmc_buffer_full && (dmc_bytes_remaining > 0));
    }
    if (dmc_refill_delay && (--dmc_refill_delay == 0)) {
        dmc_dma_request = (!dmc_buffer_full && (dmc_bytes_remaining > 0));
    }

    dmc_timer--;
    if (0 == dmc_timer) {
        dmc_timer = DMC_NTSC_RATE_CYCLES[reg_dmc_rate & 0x0F];
        if (0 == dmc_silence_flag) {
            uint8_t out_val = dmc_output + ((dmc_shift_reg & BIT_0) ? 2 : -2);
            if (out_val <= 127) {
                dmc_output = out_val;
            }
        }
        dmc_shift_reg >>= 1;
        if (--dmc_bits_remaining == 0) {
            dmc_bits_remaining = 8;
            if (!(dmc_buffer_full)) {
                dmc_silence_flag = 1;
            } else {
                dmc_silence_flag = 0;
                dmc_shift_reg = dmc_buffer;
                dmc_buffer_full = false; // mark as empty
                // No refill request while a $4015 enable is still pending: the start delay's own request
                // does the fetch (AccuracyCoin "Delta Modulation Channel" tests L-N; Mesen DMC Run()).
                if ((dmc_bytes_remaining > 0) && (dmc_start_delay == 0)) {
                    dmc_refill_delay = 1;
                }
            }
        }
    }
    length_counter_apply(&pulse_channel_1);
    length_counter_apply(&pulse_channel_2);
    length_counter_apply(&triangle_channel);
    length_counter_apply(&noise_channel);
    channels_run();
    if (mapper_ticks) {
        mapper_cpu_tick(); // (before the mix: its output of this cycle)
    }
    if (audio_on) {
        mixer_run();
    }
    apu_clock++;
}

// --- APU state log ------------------------------------------------------------------------------------------------

static FILE *apu_log = NULL;
static uint32_t apu_log_frame = 0;

void rp2a03_set_log(FILE *out)
{
    apu_log = out;
    apu_log_frame = 0;
    ppu_set_apu_log(out != NULL);
    if (out != NULL) {
        fprintf(out, "# ONE APU state, one line per frame (one --apu-log); compare with tools/apu_compare.py\n");
    }
}

static void log_field(const char *key, unsigned value)
{
    fprintf(apu_log, " %s=%u", key, value);
}

static void log_length_counter(const char *prefix, const apu_channel_t *c, unsigned enabled_bit)
{
    char key[96];
    snprintf(key, sizeof(key), "%s.counter", prefix);
    log_field(key, c->length_counter);
    snprintf(key, sizeof(key), "%s.enabled", prefix);
    log_field(key, (reg_apu_status & enabled_bit) != 0);
    snprintf(key, sizeof(key), "%s.halt", prefix);
    log_field(key, c->halt);
}

static void log_envelope(const char *prefix, const apu_channel_t *c, unsigned enabled_bit)
{
    char key[96];
    snprintf(key, sizeof(key), "%s.envelope.constantVolume", prefix);
    log_field(key, c->env_constant);
    snprintf(key, sizeof(key), "%s.envelope.counter", prefix);
    log_field(key, c->env_counter);
    snprintf(key, sizeof(key), "%s.envelope.divider", prefix);
    log_field(key, (unsigned)(uint8_t)c->env_divider);
    snprintf(key, sizeof(key), "%s.envelope.lengthCounter", prefix);
    log_length_counter(key, c, enabled_bit);
    snprintf(key, sizeof(key), "%s.envelope.start", prefix);
    log_field(key, c->env_start);
    snprintf(key, sizeof(key), "%s.envelope.volume", prefix);
    log_field(key, c->env_volume);
}

static void log_pulse(const char *prefix, const apu_channel_t *c, unsigned enabled_bit)
{
    char key[96];
#define PULSE_FIELD(name, value)                                                                                       \
    do {                                                                                                               \
        snprintf(key, sizeof(key), "%s." name, prefix);                                                                \
        log_field(key, (unsigned)(value));                                                                             \
    } while (0)
    PULSE_FIELD("duty", c->duty);
    PULSE_FIELD("dutyPos", c->duty_pos);
    log_envelope(prefix, c, enabled_bit);
    PULSE_FIELD("realPeriod", c->real_period);
    PULSE_FIELD("reloadSweep", c->sweep_reload);
    PULSE_FIELD("sweepDivider", c->sweep_divider);
    PULSE_FIELD("sweepEnabled", c->sweep_enabled);
    PULSE_FIELD("sweepNegate", c->sweep_negate);
    PULSE_FIELD("sweepPeriod", c->sweep_period);
    PULSE_FIELD("sweepShift", c->sweep_shift);
    PULSE_FIELD("sweepTargetPeriod", c->sweep_target);
    PULSE_FIELD("timer.lastOutput", c->output);
    PULSE_FIELD("timer.period", c->timer_period);
    PULSE_FIELD("timer.timer", c->timer);
#undef PULSE_FIELD
}

void rp2a03_frame_end(void)
{
    if (apu_log == NULL) {
        return;
    }
    apu_log_frame++;
    fprintf(apu_log, "frame=%u cycle=%" PRIu64, apu_log_frame, cpu_cycle);
    // the same keys as Mesen's log, in the same (alphabetical) order
    log_field("apu.dmc.bitsRemaining", dmc_bits_remaining);
    log_field("apu.dmc.bufferEmpty", !dmc_buffer_full);
    log_field("apu.dmc.bytesRemaining", dmc_bytes_remaining);
    log_field("apu.dmc.currentAddr", dmc_address);
    log_field("apu.dmc.irqEnabled", (reg_dmc_rate & BIT_7) != 0);
    log_field("apu.dmc.loopFlag", (reg_dmc_rate & BIT_6) != 0);
    log_field("apu.dmc.outputLevel", dmc_output);
    log_field("apu.dmc.readBuffer", dmc_buffer);
    log_field("apu.dmc.sampleAddr", reg_dmc_sample_addr);
    log_field("apu.dmc.sampleLength", reg_dmc_sample_len);
    log_field("apu.dmc.shiftRegister", dmc_shift_reg);
    log_field("apu.dmc.silenceFlag", dmc_silence_flag);
    log_field("apu.dmc.timer.lastOutput", dmc_output);
    log_field("apu.dmc.timer.period", (unsigned)(DMC_NTSC_RATE_CYCLES[reg_dmc_rate & 0x0F] - 1));
    log_field("apu.dmc.timer.timer", (unsigned)(dmc_timer - 1));
    log_field("apu.frameCounter.inhibitIRQ", (reg_frame_counter & FRAME_COUNTER_IRQ_INHIBIT) != 0);
    log_field("apu.frameCounter.irqFlag", frame_interrupt_flag != 0);
    log_field("apu.frameCounter.stepMode", (reg_frame_counter & FRAME_COUNTER_MODE_5_STEP) != 0);
    log_envelope("apu.noise", &noise_channel, BIT_3);
    log_field("apu.noise.modeFlag", noise_channel.mode_flag);
    log_field("apu.noise.shiftRegister", noise_channel.shift_register);
    log_field("apu.noise.timer.lastOutput", noise_channel.output);
    log_field("apu.noise.timer.period", noise_channel.timer_period);
    log_field("apu.noise.timer.timer", noise_channel.timer);
    log_pulse("apu.square1", &pulse_channel_1, BIT_0);
    log_pulse("apu.square2", &pulse_channel_2, BIT_1);
    log_length_counter("apu.triangle.lengthCounter", &triangle_channel, BIT_2);
    log_field("apu.triangle.linearControlFlag", triangle_channel.linear_control);
    log_field("apu.triangle.linearCounter", triangle_channel.linear_counter);
    log_field("apu.triangle.linearCounterReload", triangle_channel.linear_reload);
    log_field("apu.triangle.linearReloadFlag", triangle_channel.linear_reload_flag);
    log_field("apu.triangle.sequencePosition", triangle_channel.sequence_pos);
    log_field("apu.triangle.timer.lastOutput", triangle_channel.output);
    log_field("apu.triangle.timer.period", triangle_channel.timer_period);
    log_field("apu.triangle.timer.timer", triangle_channel.timer);
    fputc('\n', apu_log);
}

// --- save states -------------------------------------------------------------------------------------------------

void rp2a03_state(state_io *io)
{
    STATE(io, apu_clock);
    STATE(io, joy1_strobe);
    STATE(io, joy1_latch);
    STATE(io, joy2_strobe);
    STATE(io, joy2_latch);
    STATE(io, joy_write_pending);
    STATE(io, joy_write_value);
    STATE(io, joy_write_cycle);
    STATE(io, reg_dmc_rate);
    STATE(io, reg_dmc_sample_addr);
    STATE(io, reg_dmc_sample_len);
    STATE(io, reg_apu_status);
    STATE(io, dmc_timer);
    STATE(io, dmc_output);
    STATE(io, dmc_silence_flag);
    STATE(io, dmc_shift_reg);
    STATE(io, dmc_address);
    STATE(io, dmc_bits_remaining);
    STATE(io, dmc_bytes_remaining);
    STATE(io, dmc_irq);
    STATE(io, dmc_buffer);
    STATE(io, dmc_buffer_full);
    STATE(io, oam_dma_pending);
    STATE(io, oam_dma_page);
    STATE(io, dmc_dma_request);
    STATE(io, dmc_start_delay);
    STATE(io, dmc_disable_delay);
    STATE(io, dmc_refill_delay);
    STATE(io, reg_frame_counter);
    STATE(io, frame_counter);
    STATE(io, frame_counter_reset);
    STATE(io, frame_interrupt_flag);
    STATE(io, frame_flag_clear_cycle);
    // the channels are saved whole, without their name (a pointer: saved as NULL, put back after loading)
    apu_channel_t *channels[4] = {&pulse_channel_1, &pulse_channel_2, &triangle_channel, &noise_channel};
    const char *names[4];
    for (int i = 0; i < 4; i++) {
        names[i] = channels[i]->name;
        channels[i]->name = NULL;
    }
    STATE(io, pulse_channel_1);
    STATE(io, pulse_channel_2);
    STATE(io, triangle_channel);
    STATE(io, noise_channel);
    for (int i = 0; i < 4; i++) {
        channels[i]->name = names[i];
    }
    // not the machine: the sound output and the --apu-log file
    STATE_SKIP(dma_seen); // debugger information (the last OAM DMA)
    STATE_SKIP(dma_last_page);
    STATE_SKIP(dma_last_frame);
    STATE_SKIP(expansion_level); // (a mapper pushes it again: rp2a03_set_expansion())
    STATE_SKIP(audio_on);
    STATE_SKIP(audio_phase);
    STATE_SKIP(audio_step);
    STATE_SKIP(audio_sum);
    STATE_SKIP(audio_count);
    STATE_SKIP(mix_key);
    STATE_SKIP(mix_level);
    STATE_SKIP(apu_log);
    STATE_SKIP(apu_log_frame);
}
