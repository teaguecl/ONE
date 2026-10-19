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
#include <inttypes.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include "bits.h" // IWYU pragma: keep (the BIT_ macros are used in rp2a03.c, #included below)
#include "libco/libco.h"
#include "cpu.h"
#include "scheduler.h"
#include "log.h"
#include "timing.h"
#include "input.h"
#include "trace.h"
#include "mapper.h"
#include "ppu.h"
#include "savestate.h"
#include "script.h"
#ifdef ONE_DEBUGGER
#include "debug_hooks.h"
// Debugger build only (one-debugger): report CPU memory accesses for memory breakpoints.
#define DBG_ACCESS(address, value, is_write)                                                                           \
    do {                                                                                                               \
        if (dbg_access_hook)                                                                                           \
            dbg_access_hook((uint16_t)(address), (uint8_t)(value), (is_write));                                        \
    } while (0)
#else
#define DBG_ACCESS(address, value, is_write) ((void)0)
#endif

uint64_t glb_clock_stop = 0; // -k: stop after this CPU cycle (0 = never); set by one.c
static bool cpu_stop = false;

void cpu_request_stop(void)
{
    cpu_stop = true;
}

// (not reformatted: the columns are the low opcode nibble)
// clang-format off
const uint8_t opcode_cycles[256] = {
//  0 1 2 3 4 5 6 7 8 9 A B C D E F
//  --------------------------------
    7,6,0,8,3,3,5,5,3,2,2,2,4,4,6,6, // | 0
    0,5,0,8,4,4,6,6,2,4,2,7,4,4,7,7, // | 1
    6,6,0,8,3,3,5,5,4,2,2,2,4,4,6,6, // | 2
    0,5,0,8,4,4,6,6,2,4,2,7,4,4,7,7, // | 3
    6,6,0,8,3,3,5,5,3,2,2,2,3,4,6,6, // | 4
    0,5,0,8,4,4,6,6,2,4,2,7,4,4,7,7, // | 5
    6,6,0,8,3,3,5,5,4,2,2,2,5,4,6,6, // | 6
    0,5,0,8,4,4,6,6,2,4,2,7,4,4,7,7, // | 7
    2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4, // | 8
    0,6,0,6,4,4,4,4,2,5,2,5,5,5,5,5, // | 9
    2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4, // | A
    0,5,0,5,4,4,4,4,2,4,2,4,4,4,4,4, // | B
    2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6, // | C
    0,5,0,8,4,4,6,6,2,4,2,7,4,4,7,7, // | D
    2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6, // | E
    0,5,0,8,4,4,6,6,2,4,2,7,4,4,7,7, // | F
};
// clang-format on


#define CPU_TRACE

static bool cpu_trace_enabled = false; // -t: write a Mesen-format trace line per instruction to stderr

void cpu_set_trace(bool enable)
{
    cpu_trace_enabled = enable;
}

bool cpu_trace_active(void)
{
    return cpu_trace_enabled;
}

// --trace-sample N: with PPU catch-up the PPU is behind the CPU at most instruction boundaries, so the trace line (it has the
// PPU's position) would be wrong. Instead the PPU is caught up at every N-th boundary (on average; a random interval, so a
// loop in the game cannot line up with it) and only there a line is written: a subset of the full trace, line for line.
static uint32_t trace_sample = 0;
static uint32_t trace_countdown = 0;
static uint32_t trace_rng = 1;

void cpu_set_trace_sample(uint32_t every, uint32_t seed)
{
    trace_sample = every;
    trace_countdown = 0;
    trace_rng = seed != 0 ? seed : 1;
}

static bool trace_sample_due(void)
{
    if (trace_countdown > 0) {
        trace_countdown--;
        return false;
    }
    trace_rng ^= trace_rng << 13; // (xorshift32)
    trace_rng ^= trace_rng >> 17;
    trace_rng ^= trace_rng << 5;
    trace_countdown = trace_sample / 2 + trace_rng % (trace_sample + 1);
    return true;
}
#define TRACE_TYPE_MESEN

#ifdef TRACE_TYPE_MESEN
#define INIT_P_VALUE (0x04)
#else
#define INIT_P_VALUE (0x24) // Nintendulator
#endif

#define NMI_VECTOR     (0xFFFA)
#define RESET_VECTOR   (0xFFFC)
#define IRQ_BRK_VECTOR (0xFFFE)

#define INTERNAL_RAM_SIZE (0x800)        /* 2KB of RAM internal to the NES */
uint8_t internal_ram[INTERNAL_RAM_SIZE]; /* 2KB of RAM internal to the NES */

/* Macros for handling addressing modes */
#define IMP_DUMMY_FETCH()   (cpu_read(pc))
#define ZP_GET_SRC_ADDR()   (cpu_read_pc_inc())
#define ZP_GET_SRC_DATA()   (cpu_read_zp(ZP_GET_SRC_ADDR()))
#define ZPX_GET_SRC_ADDR()  ((cpu_read_pc_inc() + reg_x) & 0xFF)
#define ZPX_GET_SRC_DATA()  (cpu_read_zp(ZPX_GET_SRC_ADDR()))
#define ZPY_GET_SRC_ADDR()  ((cpu_read_pc_inc() + reg_y) & 0xFF)
#define ZPY_GET_SRC_DATA()  (cpu_read_zp(ZPY_GET_SRC_ADDR()))
#define INDX_GET_SRC_ADDR() (cpu_read_word_zp(cpu_read_pc_inc() + reg_x))
#define INDX_GET_SRC_DATA() (cpu_read(INDX_GET_SRC_ADDR()))
#define INDY_GET_SRC_ADDR() (cpu_read_word_zp(cpu_read_pc_inc()) + reg_y)
#define INDY_GET_SRC_DATA() (cpu_read(INDY_GET_SRC_ADDR()))

#define IMM_GET_SRC_DATA() (cpu_read_pc_inc())
//#define IND_GET_SRC_ADDR() (cpu_read_word(cpu_read_word(pc))) foobar
#define ABS_GET_SRC_ADDR() (cpu_read_abs_operand()) /* low byte first: a function call fixes the read order */
static uint16_t cpu_read_abs_operand(void);
#define ABS_GET_SRC_DATA()  (cpu_read(ABS_GET_SRC_ADDR()))
#define ABSX_GET_SRC_ADDR() ((ABS_GET_SRC_ADDR()) + reg_x)
#define ABSX_GET_SRC_DATA() (cpu_read(ABSX_GET_SRC_ADDR()))
#define ABSY_GET_SRC_ADDR() (ABS_GET_SRC_ADDR() + reg_y)
#define ABSY_GET_SRC_DATA() (cpu_read(ABSY_GET_SRC_ADDR()))

#ifdef DEBUGGING_ENABLED
#define DEBUG_ZP_GET_SRC_ADDR() (cpu_debug_read(pc))
// TODO: implement the rest of these
#endif //DEBUGGING_ENABLED

#define PUSH_WORD(VAL)                                                                                                 \
    PUSH_BYTE(((VAL) & (0xFF00)) >> 8);                                                                                \
    PUSH_BYTE((VAL) & 0x00FF)
#define PUSH_BYTE(VAL) (cpu_write_internal(0x100 + sp--, (VAL)))

//#define POP_WORD() (((uint16_t)(stack_ram[SP+2])<<8) | ((UINT16)stack_ram[SP+1])); SP += 2
#define POP_BYTE() (cpu_read_internal(++sp + 0x100))

/* Forward declarations */
void cpu_entry(void);
bool cpu_run();
static void cpu_add_cycles(uint32_t cycles);
static void cpu_write_internal(uint16_t address, uint8_t value);
static void cpu_nmi();
static uint8_t cpu_bus_read(uint16_t address);
static uint8_t cpu_bus_access(uint16_t address);
static uint8_t cpu_dma_read(uint16_t address, uint16_t *prev, bool internal_reads);
static uint16_t cpu_read_word_zp(uint8_t address);
static uint8_t cpu_read_pc_inc();

static uint16_t pc; /* program counter */
static uint8_t reg_a;
static uint8_t reg_x;
static uint8_t reg_y;
static uint8_t sp;    /* stack pointer */
static uint8_t reg_p; /* status register */

static bool previous_nmi_requested;   // external input for peripherals to request nmi
static bool nmi_requested;            // external input for peripherals to request nmi
static bool nmi_edge_detect_internal; // internal latch of the nmi edge detector
static bool nmi_edge_detect_output;   // output of the high-to-low edge detector on 'nmi_requested'
static bool nmi_poll_ready; // NEW: what cpu_run()'s poll actually sees — one cycle behind nmi_edge_detect_output

static unsigned int irq_requests; // Number of entities that have requested /IRQ go low
// Last value on the CPU data bus (every read and write, including internal RAM, stack and DMA). Reads of addresses
// nothing drives -- write-only APU/IO registers, unmapped $4018-$5FFF, and the undriven bits of $4015/$4016/$4017 --
// return it. (test_cpu_exec_space_apu: JMP $4000 fetches its opcode from write-only $4000 and gets $40 = RTI.)
static uint8_t cpu_open_bus = 0;
// The 2A03's internal data bus: the same as cpu_open_bus except that a read of $4015 (an internal register) only
// updates this one, and a DMA read that goes to the external bus only updates cpu_open_bus. Only $4015 bit 5 reads
// it. (AccuracyCoin "Internal Data Bus"; Mesen 2.2 OpenBusHandler.)
static uint8_t cpu_internal_bus = 0;

uint8_t cpu_get_open_bus(void)
{
    return cpu_open_bus;
}
static int start_state[6] = {-1, -1, -1, -1, -1, -1};
static uint64_t soft_reset_at =
    0; // command line -r: press RESET at the first instruction boundary at/after this cycle   // pc, a, x, y, s, p overrides (-1 = none)
// IRQ polling: at the end of every CPU cycle the CPU latches whether an IRQ is pending and not masked (irq_run);
// an instruction takes the IRQ if that was true at the end of its second-to-last cycle (irq_prev_run). The I flag
// itself changes immediately (CLI/SEI/PLP), which gives the usual one-instruction delay after CLI/SEI/PLP while an
// IRQ taken right after SEI still pushes I=1 (cpu_interrupts_v2/1-cli_latency vs Mesen).
static bool irq_run = false;
static bool irq_prev_run = false;

#define BIT_7 (0x80)
#define BIT_6 (0x40)
#define BIT_5 (0x20)
#define BIT_4 (0x10)
#define BIT_3 (0x08)
#define BIT_2 (0x04)
#define BIT_1 (0x02)
#define BIT_0 (0x01)

// Values for the Processor Status Reg (reg_p)
#define N_FLAG (BIT_7)
#define V_FLAG (BIT_6)
#define B_FLAG (BIT_4)
#define D_FLAG (BIT_3)
#define I_FLAG (BIT_2)
#define Z_FLAG (BIT_1)
#define C_FLAG (BIT_0)

static uint8_t last_n_value; /* most recent data which affected the N flag in the status register */
static uint8_t last_z_value; /* most recent data which affected the Z flag in the status register */

extern cothread_t ppu_thread;
cothread_t cpu_thread;
// ---- PPU catch-up (FEATURE_PPU_CATCHUP) ----------------------------------------------
// The CPU runs ahead of the PPU while nothing it does can observe the PPU: the PPU clocks it owes pile up in
// ppu_clock_requested, and the PPU runs them in one go at the next point where it matters, so that the PPU is exactly
// where it would have been: before a PPU register access, a mapper write, a DMA, a state save (and, in the debugger,
// every instruction boundary), and one CPU cycle before the PPU could change the /NMI line or the mapper's IRQ
// (ppu_catchup_limit(): VBL set / cleared; MMC3: the counted A12 rises that could bring the counter to 0).
// Where the CPU is not ahead, nothing here costs more than a comparison.
#ifdef FEATURE_PPU_CATCHUP
static bool catchup_on = true;    // ONE_NO_CATCHUP in the environment: off (to compare, or to find a bug)
static int32_t catchup_limit = 0; // PPU clocks the CPU may be ahead, from where the PPU is parked (ppu_catchup_limit())
static bool catchup_dma = false;  // a DMA is running: every cycle may touch the PPU
static uint64_t catchup_cycles = 0; // statistics (ONE_CATCHUP_STATS in the environment)
static uint64_t catchup_flushes = 0;
static bool catchup_stats = false;

static void catchup_report(void)
{
    LOG_INFO(EMU, "catch-up: %" PRIu64 " CPU cycles ran ahead of the PPU, %" PRIu64 " catch-ups", catchup_cycles,
             catchup_flushes);
}

static void catchup_init(void)
{
    static bool done = false;
    if (!done) {
        done = true;
        catchup_on = getenv("ONE_NO_CATCHUP") == NULL;
        catchup_stats = getenv("ONE_CATCHUP_STATS") != NULL;
        if (catchup_stats) {
            atexit(catchup_report);
        }
    }
}

// Let the PPU run what it owes
static void cpu_flush_ppu(void)
{
    if (ppu_add_cycles(0)) {
        catchup_flushes++;
        co_switch(ppu_thread);
        catchup_limit = ppu_catchup_limit();
    }
}
#define CATCHUP_FLUSH() cpu_flush_ppu()
// A write that can change what the limit depends on (PPUCTRL / PPUMASK: the picture and the sprite size; the mapper: its
// IRQ) has been made: work the limit out again (the PPU is caught up: the flush before the write)
#define CATCHUP_RELIMIT() (catchup_limit = ppu_catchup_limit())
#else
#define CATCHUP_FLUSH()   ((void)0)
#define CATCHUP_RELIMIT() ((void)0)
#endif
static uint32_t frame_clock; // PPU clocks into the current frame: polls the input once a frame
static uint64_t cpu_cycle;   // CPU cycles since the (last) reset
static uint32_t cpu_clock_odd_tick;

static bool cpu_stepping = false;

#include "rp2a03.c" // NOLINT(bugprone-suspicious-include): cpu and 2A03 are the same chip

// The /NMI sample of a CPU cycle, taken after the first of its 3 PPU dots: the edge detector, and what an
// instruction's interrupt poll sees. Measured against Mesen (ppu_vbl_nmi/04 + 05): with H = the dot an instruction
// starts at, /NMI raised at H-1..H+1 is detected in the instruction's cycle 1 and at H+2..H+4 in cycle 2.
static bool nmi_sample_pending = false; // the PPU is running a cycle's dots and owes the sample after the first
static void cpu_sample_nmi(void)
{
    // edge detect output is asserted during phi1 (first half of cpu cycle): an edge detected in the previous cycle
    // becomes visible to this cycle's interrupt poll.
    if (nmi_edge_detect_internal) {
        nmi_edge_detect_output = true;
    }
    nmi_poll_ready = nmi_edge_detect_output;        // what an instruction's interrupt poll sees
    if (!previous_nmi_requested && nmi_requested) { // high to low transition
        nmi_edge_detect_internal = true;
    }
    previous_nmi_requested = nmi_requested; // save a sample for next iteration
}

// Called by the PPU where it hands the CPU its turn. Right after a cycle's first dot the CPU would only take the
// /NMI sample and give the PPU the cycle's other 2 dots: done here instead, saving two coroutine switches per CPU
// cycle (about 18% of the run time). True: the PPU keeps running; false: switch to the CPU.
bool cpu_ppu_turn(void)
{
    if (!nmi_sample_pending) {
        return false;
    }
    nmi_sample_pending = false;
    cpu_sample_nmi();
    return ppu_add_cycles(8);
}

static inline uint8_t absx_get_src_data_page_cross(void)
{
    uint16_t _src_addr_base = ABS_GET_SRC_ADDR();
    uint16_t _src_addr = _src_addr_base + reg_x;
    uint8_t value;

    // first read is at the un-carried address (base high byte : low byte + index)
    value = cpu_read((_src_addr_base & 0xFF00) | (_src_addr & 0x00FF));
    if ((_src_addr_base ^ _src_addr) & 0xFF00) {
        // page crossed: that was a dummy read, the real read happens one cycle later
        value = cpu_read(_src_addr);
    }
    return value;
}

static inline uint8_t absy_get_src_data_page_cross(void)
{
    uint16_t _src_addr_base = ABS_GET_SRC_ADDR();
    uint16_t _src_addr = _src_addr_base + reg_y;
    uint8_t value;

    // first read is at the un-carried address (base high byte : low byte + index)
    value = cpu_read((_src_addr_base & 0xFF00) | (_src_addr & 0x00FF));
    if ((_src_addr_base ^ _src_addr) & 0xFF00) {
        // page crossed: that was a dummy read, the real read happens one cycle later
        value = cpu_read(_src_addr);
    }
    return value;
}

static inline uint8_t indy_get_src_data_page_cross(void)
{
    uint16_t _src_addr_base = cpu_read_word_zp(cpu_read_pc_inc());
    uint16_t _src_addr = _src_addr_base + reg_y;
    uint8_t value;

    // first read is at the un-carried address (base high byte : low byte + index)
    value = cpu_read((_src_addr_base & 0xFF00) | (_src_addr & 0x00FF));
    if ((_src_addr_base ^ _src_addr) & 0xFF00) {
        // page crossed: that was a dummy read, the real read happens one cycle later
        value = cpu_read(_src_addr);
    }
    return value;
}

void cpu_init(void)
{
    /* Create (or re-create) the cpu cooperative thread */
    if (NULL != cpu_thread) {
        co_delete(cpu_thread);
    }
    cpu_thread = co_create(65536 * sizeof(void *), cpu_entry);
    cpu_stop = false; // the debugger can power-cycle after an emulation run has ended
}

// Reference: http://wiki.nesdev.com/w/index.php/CPU_power_up_state
void cpu_power_on(void)
{
    reg_a = reg_x = reg_y = 0;
    sp = 0;       // will be decremented by 3 at cpu_reset(), giving it the default value of 0xFD
    reg_p = 0x30; // will be OR'd with 0x04 at cpu_reset(), giving it the default value of 0x34

    last_n_value = 1; // Must be non-negative, and non-zero to not interfere with initial reg_p value
    last_z_value = 1; // Must be non-negative, and non-zero to not interfere with initial reg_p value
    int i;

    /* fill internal ram with it's power on default */
    for (i = 0; i < INTERNAL_RAM_SIZE; i++) {
        internal_ram[i] = 0xFF;
    }
    internal_ram[0x0008] = 0xF7;
    internal_ram[0x0009] = 0xEF;
    internal_ram[0x000A] = 0xDF;
    internal_ram[0x000F] = 0xBF;
#ifdef CPU_TRACE
    // Nintendulator does the wrong thing at powerup, so we need to also
    for (i = 0; i < INTERNAL_RAM_SIZE; i++) {
        internal_ram[i] = 0x00;
    }
    //reg_a = reg_y = 0x7F;
    //reg_x = 0x40;
    //reg_p = 0xEF;
    //last_n_value = 0xFF;
    //last_z_value = 0x0;
    //sp = 0x8D;

    reg_p = INIT_P_VALUE;
    last_n_value = 0x24;
    last_z_value = 0x24;
#endif
}

void cpu_reset()
{
    frame_clock = 0;
    cpu_cycle = 0;
    cpu_clock_odd_tick = 0;
    nmi_requested = previous_nmi_requested = false;
    nmi_edge_detect_internal = nmi_edge_detect_output = false;

    sp -= 3;         // decremented by 3 at reset, see blargg test rom cpu_reset/registers.nes
    reg_p |= I_FLAG; // I flag set at reset, see blargg test rom cpu_reset/registers.nes
}

void cpu_nmi_request(bool request)
{
    // /NMI line level (true = asserted). Edge detection in cpu_add_cycles() decides whether an NMI happens, so a
    // pulse that rises and falls between two edge samples (e.g. a $2002 read right after VBL sets) is simply missed.
    nmi_requested = request;
}

// -r may be given several times: each value is counted from the previous reset (Mesen restarts its cycle count).
static uint64_t soft_reset_queue[8];
static int soft_reset_count = 0;
static int soft_reset_next = 0;
void cpu_set_soft_reset_cycle(uint64_t cycle)
{
    if (soft_reset_count == 0) {
        soft_reset_at = cycle;
    }
    if (soft_reset_count < 8) {
        soft_reset_queue[soft_reset_count++] = cycle;
    }
}

void cpu_set_start_state(int pc, int a, int x, int y, int s, int p)
{
    start_state[0] = pc;
    start_state[1] = a;
    start_state[2] = x;
    start_state[3] = y;
    start_state[4] = s;
    start_state[5] = p;
}

bool cpu_irq_requested(uint8_t mask)
{
    return (irq_requests & mask) != 0;
}

void cpu_irq_request(bool request, uint8_t mask)
{
    if (request) {
        irq_requests |= mask;
    } else {
        irq_requests &= ~mask;
    }
}


// Interrupt hijacking: BRK and IRQ pick their vector after pushing the return address. If an NMI edge has been
// detected by then, they use the NMI vector instead and the NMI is consumed (not taken again after the handler's
// first instruction). (cpu_interrupts_v2/2-nmi_and_brk, 3-nmi_and_irq vs Mesen.)
static uint16_t cpu_brk_irq_vector(void)
{
    if (nmi_edge_detect_internal) {
        nmi_edge_detect_internal = nmi_edge_detect_output = false;
        nmi_poll_ready = false;
        return NMI_VECTOR;
    }
    return IRQ_BRK_VECTOR;
}

//http://wiki.nesdev.com/w/index.php/CPU_status_flag_behavior
static void cpu_nmi()
{
    uint8_t temp_status_reg;

    cpu_read(pc);                  // cycle 1 (dummy)
    cpu_read(pc);                  // cycle 2 (dummy)
    PUSH_BYTE((pc & 0xFF00) >> 8); // push PCH on stack, decrement S  - cycle 3
    PUSH_BYTE(pc & 0x00FF);        // push PCL on stack, decrement S  - cycle 4
    LOG_TRACE(CPU, "NMI: pushed PC=%04X", pc);

    // re-build reg_p from the "last" variables
    reg_p &= (~(N_FLAG | Z_FLAG));   // Clear the NZ flags
    reg_p |= (last_n_value & BIT_7); // If last value was negative, set N_FLAG
    if (last_z_value == 0) {
        reg_p |= Z_FLAG;
    }
    //note the reg_p is NOT modified here.
    temp_status_reg = reg_p | BIT_5; // bit 5 always high
    temp_status_reg &= ~BIT_4;       // bit 4 low if from interrupt
    PUSH_BYTE(temp_status_reg);      // cycle 5
    LOG_TRACE(CPU, "NMI: pushed P=%02X", temp_status_reg);

    pc = cpu_read(NMI_VECTOR);           // cycle 6
    pc |= cpu_read(NMI_VECTOR + 1) << 8; // cycle 7
    reg_p |= I_FLAG;
    script_event(SCRIPT_EVENT_NMI); // (scripts see PC = the handler)
#ifdef CPU_TRACE
#ifdef TRACE_TYPE_MESEN
#endif // TRACE_TYPE_MESEN
#endif // CPU_TRACE
}

static void cpu_irq()
{ // TODO: there is a lot of overlap between this and cpu_nmi()
    uint8_t temp_status_reg;

    LOG_TRACE(CPU, "IRQ at PC=%04X", pc);
    cpu_read(pc);                  // cycle 1 (dummy)
    cpu_read(pc);                  // cycle 2 (dummy)
    PUSH_BYTE((pc & 0xFF00) >> 8); // push PCH on stack, decrement S  - cycle 3
    PUSH_BYTE(pc & 0x00FF);        // push PCL on stack, decrement S  - cycle 4

    // re-build reg_p from the "last" variables
    reg_p &= (~(N_FLAG | Z_FLAG));   // Clear the NZ flags
    reg_p |= (last_n_value & BIT_7); // If last value was negative, set N_FLAG
    if (last_z_value == 0) {
        reg_p |= Z_FLAG;
    }
    //note the reg_p is NOT modified here.
    temp_status_reg = reg_p | BIT_5; // bit 5 always high
    temp_status_reg &= ~BIT_4;       // bit 4 low if from interrupt
    {
        uint16_t vector = cpu_brk_irq_vector(); // an NMI detected by now hijacks the IRQ
        PUSH_BYTE(temp_status_reg);             // cycle 5
        pc = cpu_read(vector);                  // cycle 6
        pc |= cpu_read(vector + 1) << 8;        // cycle 7
    }
    reg_p |= I_FLAG;
    script_event(SCRIPT_EVENT_IRQ);
}

// DMA read cycle (Mesen 2.2's ProcessDmaRead). If the CPU was halted while reading $4000-$401F (internal_reads), the
// 2A03 also reads its internal register $4000 | (addr & $1F) in the same cycle -- $4015/$4016/$4017 then see a
// read (frame IRQ cleared, controller clocked) and the value can be corrupted. Otherwise the readable 2A03 registers
// ($4015-$401A) are not on the external bus and the DMA gets open bus. *prev is the last internal register address
// read (a second read of the same controller port in a row is not seen by the controller). No cycle is added here.
static uint8_t cpu_dma_read(uint16_t address, uint16_t *prev, bool internal_reads)
{
    uint8_t value;
    if (!internal_reads) {
        value = ((address >= 0x4015) && (address <= 0x401A)) ? cpu_open_bus : cpu_bus_access(address);
        *prev = address;
        return value;
    }
    uint16_t internal = 0x4000 | (address & 0x1F);
    switch (internal) {
    case 0x4015:
        value = cpu_bus_access(internal); // internal bus only
        if (internal != address) {
            uint8_t saved_internal = cpu_internal_bus;
            cpu_bus_access(address);           // the external bus is read too...
            cpu_internal_bus = saved_internal; // ...and only the external bus sees that value
        }
        break;
    case 0x4016:
    case 0x4017:
        value = (*prev == internal) ? cpu_open_bus : cpu_bus_access(internal);
        if (internal != address) {
            const uint8_t ob_mask = 0xE0; // controller port bits nothing drives (NES-001)
            uint8_t external = cpu_bus_access(address);
            cpu_open_bus = (external & ob_mask) | (value & ~ob_mask);     // the joypad drives its bits longer
            value = (external & ob_mask) | (value & external & ~ob_mask); // bus conflict
        }
        break;
    default: value = cpu_bus_access(address); break;
    }
    *prev = internal;
    return value;
}

// DMC DMA that halts the CPU on a read of cpu_address (Mesen's ProcessPendingDma without OAM DMA).
static void cpu_dmc_dma(uint16_t cpu_address)
{
    uint16_t prev = cpu_address;
    bool internal_reads = ((cpu_address & 0xFFE0) == 0x4000);
    // Controller ports: /OE stays asserted across the DMA's repeated reads, so the controllers only see the halt
    // cycle's read -- and not even that if the DMC address has the same low 5 bits (the DMA read then continues
    // the same /OE pulse). Other addresses are re-read on every idle cycle ($2007: see ppu_read()).
    bool joypad = (cpu_address == 0x4016) || (cpu_address == 0x4017);
    if (joypad && ((rp2a03_dmc_dma_address() & 0x1F) == (cpu_address & 0x1F))) {
        cpu_add_cycles(CPU_ONE_CYCLE); // halt cycle, no controller clock
    } else {
        cpu_bus_read(cpu_address); // halt cycle
    }
    // Explicit abort ($4015 disable taking effect while the DMA runs; AccuracyCoin "Explicit DMA Abort", Mesen
    // StopDmcTransfer): during the halt cycle it ends the DMA right there (1 cycle); later, the DMA still runs one
    // more cycle and then stops -- so an abort during the dummy cycle skips the read if an alignment cycle follows.
    if (!rp2a03_dmc_dma_pending()) {
        return;
    }
    if (joypad)
        cpu_add_cycles(CPU_ONE_CYCLE);
    else
        cpu_bus_read(cpu_address); // dummy cycle
    bool aborted = !rp2a03_dmc_dma_pending();

    if (0 == (cpu_cycle & 1)) { // the DMA read must land on an even cycle number (Mesen)
        if (joypad)
            cpu_add_cycles(CPU_ONE_CYCLE);
        else
            cpu_bus_read(cpu_address); // alignment cycle
        if (aborted) {
            return;
        }
    }
    uint8_t value = cpu_dma_read(rp2a03_dmc_dma_address(), &prev, internal_reads);
    cpu_add_cycles(CPU_ONE_CYCLE);
    rp2a03_dmc_dma_complete(value);
}

// Any DMA (OAM or DMC) waiting for the CPU's next read cycle?
static inline bool cpu_dma_pending(void)
{
    return rp2a03_oam_dma_pending() || rp2a03_dmc_dma_pending();
}

// Halt the CPU on a read of cpu_address and run the pending DMA(s). An OAM DMA handles a DMC fetch inside it.
static void cpu_dma(uint16_t cpu_address)
{
    CATCHUP_FLUSH();
#ifdef FEATURE_PPU_CATCHUP
    catchup_dma = true;
#endif
    if (rp2a03_oam_dma_pending()) {
        rp2a03_oam_dma(cpu_address);
    } else {
        cpu_dmc_dma(cpu_address);
    }
#ifdef FEATURE_PPU_CATCHUP
    catchup_dma = false;
#endif
}

/* read from program counter, then increment it */
static uint8_t cpu_read_pc_inc()
{
    uint8_t value = cpu_read(pc++);
    return value;
}

uint8_t cpu_read_zp(uint16_t address)
{
    uint8_t value;
    assert(address < 0x100);
    if (cpu_dma_pending()) { // a DMA halts the CPU on any read cycle
        cpu_dma(address);
    }
    value = internal_ram[address];
    SCRIPT_READ(address, value);
    cpu_open_bus = cpu_internal_bus = value;
    cpu_add_cycles(CPU_ONE_CYCLE);
    DBG_ACCESS(address, value, false);
    return value;
}

// RAM read with DMC DMA halt check (stack pulls, dummy stack reads, ...)
static uint8_t cpu_ram_read(uint16_t address);
uint8_t cpu_read_internal(uint16_t address)
{
    if (cpu_dma_pending()) { // a DMA halts the CPU on any read cycle
        cpu_dma(address);
    }
    return cpu_ram_read(address);
}

// raw RAM read, no DMA check (used by cpu_bus_read(), i.e. also by the DMA's own reads)
static uint8_t cpu_ram_read(uint16_t address)
{
    uint8_t value;
    assert(address < INTERNAL_RAM_SIZE);
    value = internal_ram[address];
    SCRIPT_READ(address, value);
    cpu_open_bus = cpu_internal_bus = value;
    cpu_add_cycles(CPU_ONE_CYCLE);
    DBG_ACCESS(address, value, false);
    return value;
}

uint8_t cpu_read(uint16_t address)
{
    if (cpu_dma_pending()) {
        cpu_dma(address);
    }
    return cpu_bus_read(address);
}

// One CPU bus access without the cycle. Every read drives both data buses except $4015: that register is inside the
// 2A03, so its value only reaches the internal bus (Mesen: SetOpenBus(value, addr == 0x4015)).
static uint8_t cpu_bus_access(uint16_t address)
{
    uint8_t value;
    if (address < 0x2000) {
        value = internal_ram[address & 0x7FF];
    } else if (address < 0x4000) {
        CATCHUP_FLUSH();
        value = ppu_read(address);
        if (address == 0x2007) {
            CATCHUP_RELIMIT(); // (a read puts the VRAM address on the bus: A12 for the mapper)
        }
    } else if (address <= 0x4018) {
        if (address == 0x4016 || address == 0x4017) {
            CATCHUP_FLUSH(); // a poll counts for the lag counter in the frame the PPU is in at this cycle
        }
        value = rp2a03_read(address); // write-only registers return cpu_open_bus
    } else if (address < 0x6000) {
        // $4019-$5FFF: nothing drives the bus on NROM/MMC1; expansion hardware (MMC5) answers through read_exp
        value = cpu_open_bus;
        if (mapper_has_exp) {
            CATCHUP_FLUSH(); // (the MMC5's in-frame flag and IRQ come from what the PPU did)
            if (!mapper_read_exp(address, &value)) {
                value = cpu_open_bus;
            }
        }
    } else {
        value = mapper_read_prg(address);
    }
    cpu_internal_bus = value;
    if (address != 0x4015) {
        cpu_open_bus = value;
    }
    return value;
}

static uint8_t cpu_bus_read(uint16_t address)
{
    uint8_t value = cpu_bus_access(address);
    SCRIPT_READ(address, value); // (a script may change what the CPU reads; the buses keep what was driven)
    cpu_add_cycles(CPU_ONE_CYCLE);
    DBG_ACCESS(address, value, false);
    return value;
}

static uint16_t cpu_read_word_zp(uint8_t address)
{
    uint16_t value;
    uint16_t upper_byte;

    value = (uint16_t)cpu_read_zp(address & 0xFF);
    address = (address + 1) & 0xFF;
    upper_byte = ((uint16_t)cpu_read_zp(address)) << 8;
    value |= upper_byte;
    return value;
}

void cpu_write_zp(uint16_t address, uint8_t value)
{
    assert(address < 0x100);
    SCRIPT_WRITE(address, value);
    internal_ram[address] = value;
    cpu_open_bus = cpu_internal_bus = value;
    cpu_add_cycles(CPU_ONE_CYCLE);
    DBG_ACCESS(address, value, true);
}

static void cpu_write_internal(uint16_t address, uint8_t value)
{
    assert(address < INTERNAL_RAM_SIZE);
    SCRIPT_WRITE(address, value);
    internal_ram[address] = value;
    cpu_open_bus = cpu_internal_bus = value;
    cpu_add_cycles(CPU_ONE_CYCLE);
    DBG_ACCESS(address, value, true);
}

void cpu_write(uint16_t address, uint8_t value)
{
    if (address < 0x2000) {
        cpu_write_internal(address & 0x7FF, value);
        return;
    }
    SCRIPT_WRITE(address, value); // (internal RAM: in cpu_write_internal())
    if (address < 0x4000) {
        cpu_open_bus = cpu_internal_bus = value;
        CATCHUP_FLUSH();
        ppu_write(address, value);
        CATCHUP_RELIMIT();
        cpu_add_cycles(CPU_ONE_CYCLE);
        //cpu_add_cycles(CPU_ONE_CYCLE);
    } else if (address <= 0x4018) {
        // 2A03 specific stuff
        cpu_open_bus = cpu_internal_bus = value;
        if (address == 0x4016 || address == 0x4017) {
            CATCHUP_FLUSH(); // (as for the read: the frame the game polls in)
        }
        rp2a03_write(address, value);
        cpu_add_cycles(CPU_ONE_CYCLE);

    } else {
        // mapper
        cpu_open_bus = cpu_internal_bus = value;
        CATCHUP_FLUSH();
        mapper_write_prg(address, value);
        CATCHUP_RELIMIT();
        cpu_add_cycles(CPU_ONE_CYCLE);
    }
    DBG_ACCESS(address, value, true); // (internal RAM writes are reported by cpu_write_internal())
}

cothread_t cpu_get_thread()
{
    return cpu_thread;
}

static void cpu_reset_sequence(void)
{

#ifdef TRACE_TYPE_MESEN
    // for some reason Mesen starts at PPU cycle 30 (CPU cycle 10).
    // These two extra reads are here to make that line up, but are certainly wrong.
    // TODO: look up what Mesen does here
    //cpu_read(0x00ff);  // -2?
    cpu_read(0x00ff); // -1?

    // First 9 cycles are documented here: https://www.pagetable.com/?p=410
    cpu_read(0x00ff);              // 0
    cpu_read(0x00ff);              // 1
    cpu_read(0x00ff);              // 2
    cpu_read(0x0100);              // 3
    cpu_read(0x01ff);              // 4
    cpu_read(0x01fe);              // 5
    pc = cpu_read(0xfffc);         // 6
    pc |= (cpu_read(0xfffd) << 8); // 7

    LOG_DEBUG(CPU, "reset sequence done: frame_clock %u, cpu_cycle %" PRIu64, frame_clock, cpu_cycle);
    // Mesen starts the CPU at cycle 0 when PPU is at cycle 30
    frame_clock = 96;
    cpu_cycle = 8;
    cpu_clock_odd_tick = 0;

#else
    /* read new program counter from the reset vector */
    // we cheat here and go straight to the mapper, to avoid incrementing the CPU clock
    pc = mapper_read_prg(RESET_VECTOR);
    pc |= mapper_read_prg(RESET_VECTOR + 1) << 8;
#endif // TRACE_TYPE_MESEN
    script_event(SCRIPT_EVENT_RESET);
}

static void cpu_main_loop(bool boundary_hook_first);

void cpu_entry(void)
{
    cpu_reset_sequence();

    // command-line start-state overrides (-p / -R), applied after the reset sequence
    if (start_state[0] >= 0)
        pc = (uint16_t)start_state[0];
    if (start_state[1] >= 0)
        reg_a = (uint8_t)start_state[1];
    if (start_state[2] >= 0)
        reg_x = (uint8_t)start_state[2];
    if (start_state[3] >= 0)
        reg_y = (uint8_t)start_state[3];
    if (start_state[4] >= 0)
        sp = (uint8_t)start_state[4];
    if (start_state[5] >= 0) {
        reg_p = (uint8_t)start_state[5];
        last_n_value = (reg_p & N_FLAG) ? 0x80 : 0x00; // N and Z are computed lazily from these
        last_z_value = (reg_p & Z_FLAG) ? 0x00 : 0x01;
    }

    // give debugger control after reset vectoring is complete
    //co_switch(debugger_get_thread());

    cpu_main_loop(true);
}

// One instruction per iteration. A loaded save state starts a new CPU coroutine here (cpu_state_restart()).
// boundary_hook_first false: the debugger has already seen this instruction boundary (it was stopped there).
static void cpu_main_loop(bool boundary_hook_first)
{
    bool nmi_detected;
#ifdef ONE_DEBUGGER
    bool skip_hook = !boundary_hook_first;
#else
    (void)boundary_hook_first;
#endif

    while (!cpu_stop) {
        if (soft_reset_at && (cpu_cycle >= soft_reset_at)) {
            // RESET button (command line -r): PPU and APU restart like power-on, RAM and A/X/Y are kept,
            // S -= 3 and I is set (cpu_reset()), then the normal reset sequence runs (cpu_reset/ tests vs Mesen).
            soft_reset_next++;
            soft_reset_at = (soft_reset_next < soft_reset_count) ? soft_reset_queue[soft_reset_next] : 0;
            ppu_soft_reset();
#ifdef FEATURE_PPU_CATCHUP
            catchup_limit = 0; // (the PPU starts again: work the limit out at its next stop)
#endif
            rp2a03_reset();
            cpu_reset();
            irq_run = irq_prev_run = false;
            nmi_poll_ready = false;
            cpu_reset_sequence();
            continue;
        }
#ifdef ONE_DEBUGGER
        // Debugger build only: instruction boundary. The debugger may pause here (control returns to it until it
        // resumes us). A RESET requested while paused is handled before the instruction runs.
#ifdef FEATURE_PPU_CATCHUP
        CATCHUP_FLUSH(); // the debugger looks at the PPU: it is where the CPU is
#endif
        if (skip_hook) {
            skip_hook = false;
        } else if (dbg_boundary_hook && dbg_boundary_hook(pc)) {
            sched_break();
            if (cpu_stop) {
                break;
            }
            if (soft_reset_at && (cpu_cycle >= soft_reset_at)) {
                continue;
            }
        }
#endif
#ifdef FEATURE_PPU_CATCHUP
        // a request that is not due yet (a save at a frame still some way off) waits, and the CPU stays ahead of the PPU
        if (savestate_pending && savestate_wants_ppu()) {
            CATCHUP_FLUSH();
            savestate_boundary();
        }
#else
        if (savestate_pending) { // save (when the PPU is at a quiet point) or load a state
            savestate_boundary();
        }
#endif
        if (script_hook_exec && (script_watch[pc] & 4)) {
            script_mem_exec(pc); // a script's exec callback for this address
        }
        nmi_detected = cpu_run();

        if (nmi_detected) {
            LOG_DEBUG(CPU, "NMI at PC=%04X", pc);
            cpu_nmi();
            // Servicing the NMI clears the edge detector's internal signal. The /NMI line itself is a level driven by
            // the PPU (VBL flag AND PPUCTRL.7) and is NOT cleared here -- a new NMI needs a new low->high edge.
            nmi_edge_detect_internal = nmi_edge_detect_output = false;
            nmi_poll_ready = false;
        }

        else if (irq_prev_run) {
            cpu_irq();
        }

        if (cpu_stepping) {
            cpu_stepping = false;
            //co_switch(debugger_get_thread());
        }
    }
    LOG_DEBUG(EMU, "emulation stopped at CPU cycle %" PRIu64, cpu_cycle);
    sched_exit(0); // hand control back to the host thread instead of
                   // falling off the end of cpu_entry(): libco has no
                   // idea what to do with a cothread entrypoint that
                   // returns, and jumps to its crash() trampoline.
}


// returns true if NMI detected
/* SHX / SHY / SHA / TAS (unofficial "unstable" stores): the value stored is reg & (base_high + 1); the dummy read is at
 * the un-carried address; if indexing crossed a page, the high byte of the target address is replaced by the value.
 * (Matches Mesen on instr_test-v5 07-abs_xy -- SHY $02FE,X with X=2,Y=1 writes $01 to $0100 -- and on
 * instr_timing 1-instr_timing -- TAS $03FD,Y crossing into $04xx with A&X&$04 = 0 writes $00 to $0000.) */
static void cpu_unstable_store(uint16_t base, uint8_t index, uint8_t reg)
{
    uint16_t target = base + index;
    uint8_t anded = reg & (uint8_t)((base >> 8) + 1);
    uint64_t start = cpu_cycle;

    cpu_read((base & 0xFF00) | (target & 0x00FF)); // dummy read before the carry is fixed up
    // If a DMA halted the CPU on that dummy read (it took more than one cycle), the "& H" term is lost and the
    // register is stored unmodified (nesdev forum t=297765; Mesen SyaSxaAxa; AccuracyCoin SH* tests, error 7).
    // The page-cross address corruption still uses the ANDed value.
    uint8_t value = ((cpu_cycle - start) > 1) ? reg : anded;
    if ((target & 0xFF00) != (base & 0xFF00)) {
        target = ((uint16_t)anded << 8) | (target & 0x00FF); // page cross: high byte becomes reg & H
    }
    cpu_write(target, value);
}

static uint16_t cpu_read_abs_operand(void)
{
    uint16_t lo = cpu_read_pc_inc();
    uint16_t hi = cpu_read_pc_inc();
    return (hi << 8) | lo;
}

/* abs,X / abs,Y stores and RMWs: fetch the operand, then always do the dummy read at
 * the address before the high-byte carry is applied (hi_base : lo+index). */
static uint16_t cpu_abs_indexed_dummy_read(uint8_t index)
{
    uint16_t base = cpu_read_abs_operand();
    uint16_t addr = base + index;
    cpu_read((base & 0xFF00) | (addr & 0x00FF));
    return addr;
}

/* Unofficial read-modify-write opcodes: SLO RLA SRE RRA DCP ISC in modes (zp,X) zp abs (zp),Y zp,X abs,Y abs,X.
 * Opcode layout: aaa bbb 11, aaa = operation (0 SLO, 1 RLA, 2 SRE, 3 RRA, 6 DCP, 7 ISC), bbb = mode (0 (zp,X), 1 zp,
 * 3 abs, 4 (zp),Y, 5 zp,X, 6 abs,Y, 7 abs,X). They make the same bus accesses as the official RMW instructions:
 * address fetch (indexed modes always dummy-read the un-carried address), read, dummy write of the old value, write of
 * the new value. The old per-opcode handlers padded a cycle instead of doing the dummy write, so e.g. SLO $2006 wrote
 * $2006 only once (cpu_dummy_writes_ppumem vs Mesen). */
static bool cpu_is_unofficial_rmw(uint8_t opcode)
{
    uint8_t op = opcode >> 5, mode = (opcode >> 2) & 7;
    return ((opcode & 3) == 3) && (op <= 3 || op >= 6) && (mode != 2);
}

static void cpu_adc_value(uint8_t v)
{
    uint8_t a = reg_a;
    uint16_t sum = (uint16_t)a + v + (reg_p & C_FLAG);
    reg_p &= ~(C_FLAG | V_FLAG);
    if (sum > 0xFF)
        reg_p |= C_FLAG;
    if (~(a ^ v) & (a ^ sum) & 0x80)
        reg_p |= V_FLAG;
    reg_a = (uint8_t)sum;
    last_n_value = last_z_value = reg_a;
}

static void cpu_unofficial_rmw(uint8_t opcode)
{
    uint8_t op = opcode >> 5, mode = (opcode >> 2) & 7;
    uint16_t addr = 0, base;
    uint8_t zp, v, nv, c;

    switch (mode) {
    case 0: /* (zp,X) */
        zp = cpu_read_pc_inc();
        cpu_read(zp); // dummy read while X is added
        addr = cpu_read_word_zp((uint8_t)(zp + reg_x));
        break;
    case 1: /* zp */ addr = cpu_read_pc_inc(); break;
    case 3: /* abs */ addr = cpu_read_abs_operand(); break;
    case 4: /* (zp),Y */
        zp = cpu_read_pc_inc();
        base = cpu_read_word_zp(zp);
        addr = base + reg_y;
        cpu_read((base & 0xFF00) | (addr & 0x00FF)); // dummy read at the un-carried address
        break;
    case 5: /* zp,X */
        zp = cpu_read_pc_inc();
        cpu_read(zp); // dummy read while X is added
        addr = (uint8_t)(zp + reg_x);
        break;
    case 6: /* abs,Y */
    case 7: /* abs,X */
        base = cpu_read_abs_operand();
        addr = base + ((mode == 6) ? reg_y : reg_x);
        cpu_read((base & 0xFF00) | (addr & 0x00FF)); // dummy read at the un-carried address
        break;
    }

    v = cpu_read(addr);
    cpu_write(addr, v); // dummy write of the unmodified value
    c = reg_p & C_FLAG;
    switch (op) {
    case 0: /* SLO */
        nv = v << 1;
        reg_p = (reg_p & ~C_FLAG) | (v >> 7);
        break;
    case 1: /* RLA */
        nv = (v << 1) | c;
        reg_p = (reg_p & ~C_FLAG) | (v >> 7);
        break;
    case 2: /* SRE */
        nv = v >> 1;
        reg_p = (reg_p & ~C_FLAG) | (v & 1);
        break;
    case 3: /* RRA */
        nv = (v >> 1) | (c << 7);
        reg_p = (reg_p & ~C_FLAG) | (v & 1);
        break;
    case 6: /* DCP */ nv = v - 1; break;
    default: /* ISC */ nv = v + 1; break;
    }
    cpu_write(addr, nv);

    switch (op) {
    case 0:
        reg_a |= nv;
        last_n_value = last_z_value = reg_a;
        break;
    case 1:
        reg_a &= nv;
        last_n_value = last_z_value = reg_a;
        break;
    case 2:
        reg_a ^= nv;
        last_n_value = last_z_value = reg_a;
        break;
    case 3: cpu_adc_value(nv); break;
    case 6:
        reg_p &= ~C_FLAG;
        if (reg_a >= nv)
            reg_p |= C_FLAG;
        last_n_value = last_z_value = (uint8_t)(reg_a - nv);
        break;
    default: cpu_adc_value(nv ^ 0xFF); break; // ISC: SBC = ADC of the inverted value
    }
}

/* Taken branches poll for interrupts on their 2nd and 4th cycles (Mesen 2.2 BranchRelative; AccuracyCoin "Interrupt
 * flag latency" test E). Call after the operand fetch: the IRQ decision starts from the poll made before the operand
 * cycle, so an IRQ that becomes pending during the operand fetch is delayed by one instruction if the branch does not
 * cross a page (cpu_interrupts_v2/5-branch_delays_irq), and one seen by that first poll is taken even if it goes away.
 * A page-crossing branch ORs in the poll at the end of its 3rd cycle (cpu_branch_irq_poll4()). */
static void cpu_branch_irq_delay(uint16_t old_pc, uint16_t new_pc)
{
    (void)old_pc;
    (void)new_pc;
    irq_run = irq_prev_run;
}

static void cpu_branch_irq_poll4(void)
{
    irq_run |= irq_prev_run;
}

bool cpu_run()
{
    bool nmi_detect;
    uint8_t opcode;

    // A DMC DMA pending at an instruction boundary halts the CPU on this opcode fetch. Do it before the trace line
    // so the stolen cycles are counted before the instruction, as in Mesen's trace (timing is identical either way:
    // the cpu_read(pc) below would do exactly the same).
    if (cpu_dma_pending()) {
        cpu_dma(pc);
    }
#ifdef CPU_TRACE
    if (cpu_trace_enabled) {
        if (0 == trace_sample) {
            debugger_generate_trace();
        } else if (trace_sample_due()) {
            CATCHUP_FLUSH();
            debugger_generate_trace();
        }
    }
#endif

    opcode = cpu_read_pc_inc();
    if (cpu_is_unofficial_rmw(opcode)) {
        cpu_unofficial_rmw(opcode); // the per-opcode SLO/RLA/SRE/RRA/DCP/ISC cases below are no longer reached
    } else
        switch (opcode) {
        case 0x00: /* 0x00 - BRK */
        {
            cpu_read_pc_inc(); // dummy read of padding byte (cycle 2)
            PUSH_WORD(pc);     // cycles 3, 4

            // re-create reg_p
            reg_p &= ~(N_FLAG | Z_FLAG);
            reg_p |= (last_n_value & BIT_7);
            if (last_z_value == 0) {
                reg_p |= Z_FLAG;
            }
            {
                uint16_t vector = cpu_brk_irq_vector(); // an NMI detected by now hijacks the BRK
                PUSH_BYTE(reg_p | BIT_4 | BIT_5);       // cycle 5
                reg_p |= I_FLAG;
                pc = cpu_read(vector);           // cycle 6
                pc |= cpu_read(vector + 1) << 8; // cycle 7
                // An NMI detected too late to hijack still waits until the handler's first instruction has run
                // (2-nmi_and_brk vs Mesen); it stays latched in the edge detector.
                nmi_poll_ready = false;
            }
        } break;
        case 0x01: /* 0x01 - ORA INDX */
            cpu_read(pc);
            reg_a |= INDX_GET_SRC_DATA();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x03: /* 0x03 - SLO INDX */
        {
            uint16_t src_addr;
            uint8_t src_data;
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            src_addr = INDX_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            reg_p &= ~C_FLAG;
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C bit */
            src_data <<= 1;
            cpu_write(src_addr, src_data); /* write it back to memory */
            reg_a |= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x04: /* 0x04 - NOP ZP (Undocumented) */ ZP_GET_SRC_DATA(); break;
        case 0x05: /* 0x05 - ORA ZP */
            reg_a |= ZP_GET_SRC_DATA();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x06: /* 0x06 - ASL ZP */
        {
            uint8_t src_addr;
            uint8_t src_data;

            src_addr = ZP_GET_SRC_ADDR();
            src_data = cpu_read_zp(src_addr);
            cpu_write_zp(src_addr, src_data); // dummy write
            reg_p &= ~C_FLAG;
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C bit */
            src_data <<= 1;
            cpu_write_zp(src_addr, src_data); /* write it back to memory */
            last_n_value = last_z_value = src_data;
        } break;
        case 0x07: /* 0x07 - SLO ZP */
        {
            uint8_t src_addr;
            uint8_t src_data;

            src_addr = ZP_GET_SRC_ADDR();
            src_data = cpu_read_zp(src_addr);
            cpu_write_zp(src_addr, src_data); // dummy write
            reg_p &= ~C_FLAG;
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C bit */
            src_data <<= 1;
            cpu_write_zp(src_addr, src_data); /* write it back to memory */
            reg_a |= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x08: /* 0x08 - PHP IMP */
            IMP_DUMMY_FETCH();
            reg_p &= (~(N_FLAG | Z_FLAG));   // Clear the NZ flags
            reg_p |= (last_n_value & BIT_7); // If last value was negative, set N_FLAG
            if (last_z_value == 0) {
                reg_p |= Z_FLAG;
            }
            PUSH_BYTE(reg_p | BIT_4 | BIT_5); // reference: http://wiki.nesdev.com/w/index.php/Status_flags
            break;
        case 0x09: /* 0x09 - ORA IMM */
            reg_a |= IMM_GET_SRC_DATA();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x0A:        /* 0x0A - ASL ACC */
            cpu_read(pc); // dummy read
            reg_p &= ~C_FLAG;
            reg_p |= ((reg_a & BIT_7) >> 7); /* move MSB into C bit */
            reg_a <<= 1;
            last_n_value = last_z_value = reg_a;
            break;
        case 0x0B: /* 0x0B - AND IMM (Undocumented) */
        case 0x2B: /* 0x2B - AND IMM (Undocumented) */
            reg_a &= IMM_GET_SRC_DATA();
            reg_p &= ~C_FLAG;
            reg_p |= ((reg_a & BIT_7) >> 7); /* move MSB into C bit */
            last_n_value = last_z_value = reg_a;
            break;
        case 0x0C: /* 0x0C - NOP ABS (Undocumented) */ ABS_GET_SRC_DATA(); break;
        case 0x0D: /* 0x0D - ORA ABS */
            reg_a |= ABS_GET_SRC_DATA();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x0E: /* 0x0E - ASL ABS */
        {
            uint16_t src_addr;
            uint8_t src_data;

            src_addr = ABS_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data); // dummy write
            reg_p &= ~C_FLAG;
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C bit */
            src_data <<= 1;
            cpu_write(src_addr, src_data); /* write it back to memory */
            last_n_value = last_z_value = src_data;
        } break;
        case 0x0F: /* 0x0F - SLO ABS */
        {
            uint16_t src_addr;
            uint8_t src_data;
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 6 cycles total for the instruction
            src_addr = ABS_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            reg_p &= ~C_FLAG;
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C bit */
            src_data <<= 1;
            cpu_write(src_addr, src_data); /* write it back to memory */
            reg_a |= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x10: /* 0x10 - BPL REL */
        {
            int8_t branch_offset = (int8_t)cpu_read_pc_inc();
            // (the branch polls the NMI edge detector here: see nmi_poll_ready)

            if ((last_n_value & BIT_7) == 0) {
                uint16_t old_pc = pc;
                pc += branch_offset;
                cpu_branch_irq_delay(old_pc, pc);
                cpu_read(old_pc); // cycle 3: dummy read of the byte after the operand
                if ((pc ^ old_pc) & 0xFF00) {
                    // the high byte changed, indicates page crossing
                    // (the branch polls the NMI edge detector here: see nmi_poll_ready)
                    cpu_branch_irq_poll4();
                    cpu_read((old_pc & 0xFF00) | (pc & 0x00FF)); // cycle 4: dummy read before PCH is fixed
                }
            }
        } break;
        case 0x11: /* 0x11 - ORA INDY */
                   //    reg_a |= INDY_GET_SRC_DATA();
            reg_a |= indy_get_src_data_page_cross();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x14: /* 0x14 - NOP ZPX (Undocumented) */
        case 0x34: /* 0x14 - NOP ZPX (Undocumented) */
        case 0x54: /* 0x14 - NOP ZPX (Undocumented) */
        case 0x74: /* 0x14 - NOP ZPX (Undocumented) */
        case 0xD4: /* 0x14 - NOP ZPX (Undocumented) */
        case 0xF4: /* 0x14 - NOP ZPX (Undocumented) */
        {
            uint8_t addr;
            addr = cpu_read_pc_inc();
            cpu_read_zp(addr);
            addr += reg_x;
            cpu_read_zp(addr);
        } break;
        case 0x13: /* 0x13 - SLO INDY */
        {
            uint16_t src_addr;
            uint8_t src_data;
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            src_addr = INDY_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            reg_p &= ~C_FLAG;
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C bit */
            src_data <<= 1;
            cpu_write(src_addr, src_data); /* write it back to memory */
            reg_a |= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x15: /* 0x15 - ORA ZPX  */
        {
            uint8_t src_addr;

            src_addr = ZPX_GET_SRC_ADDR();
            cpu_read_zp(src_addr);
            reg_a |= cpu_read_zp(src_addr);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x16: /* 0x16 - ASL ZPX */
        {
            uint8_t src_addr;
            uint8_t src_data;

            src_addr = ZPX_GET_SRC_ADDR();
            cpu_read_zp(src_addr);
            src_data = cpu_read_zp(src_addr);
            cpu_write_zp(src_addr, src_data);
            reg_p &= ~C_FLAG;
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C bit */
            src_data <<= 1;
            cpu_write_zp(src_addr, src_data); /* write it back to memory */
            last_n_value = last_z_value = src_data;
        } break;
        case 0x17: /* 0x17 - SLO ZPX */
        {
            uint8_t src_addr;
            uint8_t src_data;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 6 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 6 cycles total for the instruction
            src_addr = ZPX_GET_SRC_ADDR();
            src_data = cpu_read_zp(src_addr);
            reg_p &= ~C_FLAG;
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C bit */
            src_data <<= 1;
            cpu_write_zp(src_addr, src_data); /* write it back to memory */
            reg_a |= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x18: /* 0x18 - CLC IMP */
            IMP_DUMMY_FETCH();
            reg_p &= ~C_FLAG;
            break;
        case 0x19: /* 0x19 - ORA ABSY */
            reg_a |= absy_get_src_data_page_cross();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x1C: /* 0x1C - NOP ABSX (Undocumented) */
        case 0x3C: /* 0x3C - NOP ABSX (Undocumented) */
        case 0x5C: /* 0x5C - NOP ABSX (Undocumented) */
        case 0x7C: /* 0x7C - NOP ABSX (Undocumented) */
        case 0xDC: /* 0xDC - NOP ABSX (Undocumented) */
        case 0xFC: /* 0xFC - NOP ABSX (Undocumented) */
        {
            uint8_t addr_low, addr_high;
            uint16_t old_addr;
            uint16_t src_addr;

            addr_low = cpu_read_pc_inc();  // 2
            addr_high = cpu_read_pc_inc(); // 3
            src_addr = (((uint16_t)(addr_high) << 8) | addr_low) + reg_x;
            addr_low += reg_x;
            old_addr = ((uint16_t)(addr_high) << 8) | addr_low;
            cpu_read(old_addr); //4
            if (old_addr != src_addr) {
                cpu_read(src_addr);
            }
        } break;
        case 0x1B: /* 0x1B - SLO ABSY */
        {
            uint16_t src_addr;
            uint8_t src_data;
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            src_addr = ABSY_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            reg_p &= ~C_FLAG;
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C bit */
            src_data <<= 1;
            cpu_write(src_addr, src_data); /* write it back to memory */
            reg_a |= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x1D: /* 0x1D - ORA ABSX  */
            reg_a |= absx_get_src_data_page_cross();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x1E: /* 0x1E - ASL ABSX */
        {
            uint8_t addr_low, addr_high;
            uint16_t old_addr;
            uint16_t src_addr;
            uint8_t src_data;

            addr_low = cpu_read_pc_inc();  // 2
            addr_high = cpu_read_pc_inc(); // 3
            src_addr = (((uint16_t)(addr_high) << 8) | addr_low) + reg_x;
            addr_low += reg_x;
            old_addr = ((uint16_t)(addr_high) << 8) | addr_low;

            cpu_read(old_addr);            //4
            src_data = cpu_read(src_addr); //5
            cpu_write(src_addr, src_data); // dummy write

            reg_p &= ~C_FLAG;
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C bit */
            src_data <<= 1;
            cpu_write(src_addr, src_data); /* write it back to memory */
            last_n_value = last_z_value = src_data;
        } break;
        case 0x1F: /* 0x1F - SLO ABSX */
        {
            uint16_t src_addr;
            uint8_t src_data;
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            src_addr = ABSX_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            reg_p &= ~C_FLAG;
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C bit */
            src_data <<= 1;
            cpu_write(src_addr, src_data); /* write it back to memory */
            reg_a |= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x20: /* 0x20 - JSR ABS */
            /* cycle timing from: http://nesdev.com/6502_cpu.txt */
            {
                uint8_t temp = cpu_read_pc_inc();                     // fetch low address byte, increment PC
                cpu_read_internal(0x100 + sp);                        // internal operation (predecrement S?)
                cpu_write_internal(0x100 + sp--, (pc & 0xFF00) >> 8); // push PCH on stack, decrement S
                cpu_write_internal(0x100 + sp--, (pc & 0x00FF));      // push PCL on stack, decrement S
                pc = (cpu_read_pc_inc() << 8) | temp;
            }
            break;
        case 0x21: /* 0x21 - AND INDX */
            cpu_read(pc);
            reg_a &= INDX_GET_SRC_DATA();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x23: /* 0x23 - RLA INDX */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            src_addr = INDX_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;                   /* clear C flag */
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C flag */
            src_data <<= 1;
            src_data |= saved_c;           /* put the old C back at bit position 0 */
            cpu_write(src_addr, src_data); /* write it back to memory */
            reg_a &= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x24: /* 0x24 - BIT ZP */
        {
            uint8_t src_data;
            src_data = ZP_GET_SRC_DATA();
            last_n_value = src_data;
            reg_p &= ~V_FLAG;
            reg_p |= (src_data & BIT_6);
            last_z_value = (src_data & reg_a);
        } break;
        case 0x25: /* 0x25 - AND ZP */
            reg_a &= ZP_GET_SRC_DATA();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x26: /* 0x26 - ROL ZP */
        {
            uint8_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;

            src_addr = ZP_GET_SRC_ADDR();
            src_data = cpu_read_zp(src_addr);
            cpu_write_zp(src_addr, src_data); // dummy write
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;                   /* clear C flag */
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C flag */
            src_data <<= 1;
            src_data |= saved_c;              /* put the old C back at bit position 0 */
            cpu_write_zp(src_addr, src_data); /* write it back to memory */
            last_n_value = last_z_value = src_data;
        } break;
        case 0x27: /* 0x27 - RLA ZP */
        {
            uint8_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 5 cycles total for the instruction
            src_addr = ZP_GET_SRC_ADDR();
            src_data = cpu_read_zp(src_addr);
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;                   /* clear C flag */
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C flag */
            src_data <<= 1;
            src_data |= saved_c;              /* put the old C back at bit position 0 */
            cpu_write_zp(src_addr, src_data); /* write it back to memory */
            reg_a &= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x28: /* 0x28 - PLP IMP */
        {
            IMP_DUMMY_FETCH();
            cpu_read_internal(sp + 0x100);           // dummy read
            reg_p = (POP_BYTE() & ~(BIT_4 | BIT_5)); // I takes effect now (see irq_prev_run for the delay)
            last_n_value = reg_p;
            last_z_value = (reg_p & Z_FLAG) ^ Z_FLAG;
        } break;
        case 0x29: /* 0x29 - AND IMM */
            reg_a &= IMM_GET_SRC_DATA();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x2A: /* 0x2A - ROL ACC */
        {
            uint8_t saved_c;
            cpu_read(pc); // dummy read
            saved_c = (reg_p & C_FLAG);

            reg_p &= ~C_FLAG;                /* clear C flag */
            reg_p |= ((reg_a & BIT_7) >> 7); /* move MSB into C flag */
            reg_a <<= 1;
            reg_a |= saved_c; /* put the old C back at bit position 0 */
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x2C: /* 0x2C - BIT ABS */
        {
            uint8_t src_data = ABS_GET_SRC_DATA();
            last_n_value = src_data;
            reg_p &= ~V_FLAG;
            reg_p |= (src_data & BIT_6);
            last_z_value = (src_data & reg_a);
        } break;
        case 0x2D: /* 0x2D - AND ABS */
            reg_a &= ABS_GET_SRC_DATA();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x2E: /* 0x2E - ROL ABS */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;

            src_addr = ABS_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data); // dummy write
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;                   /* clear C flag */
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C flag */
            src_data <<= 1;
            src_data |= saved_c;           /* put the old C back at bit position 0 */
            cpu_write(src_addr, src_data); /* write it back to memory */
            last_n_value = last_z_value = src_data;
        } break;
        case 0x2F: /* 0x2F - RLA ABS */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 6 cycles total for the instruction
            src_addr = ABS_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;                   /* clear C flag */
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C flag */
            src_data <<= 1;
            src_data |= saved_c;           /* put the old C back at bit position 0 */
            cpu_write(src_addr, src_data); /* write it back to memory */
            reg_a &= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x30: /* 0x30 - BMI REL */
        {
            int8_t branch_offset = (int8_t)cpu_read_pc_inc();
            // (the branch polls the NMI edge detector here: see nmi_poll_ready)
            if (last_n_value & BIT_7) {
                uint16_t old_pc = pc;
                pc += branch_offset;
                cpu_branch_irq_delay(old_pc, pc);
                cpu_read(old_pc); // cycle 3: dummy read of the byte after the operand
                if ((pc ^ old_pc) & 0xFF00) {
                    // the high byte changed, indicates page crossing
                    // (the branch polls the NMI edge detector here: see nmi_poll_ready)
                    cpu_branch_irq_poll4();
                    cpu_read((old_pc & 0xFF00) | (pc & 0x00FF)); // cycle 4: dummy read before PCH is fixed
                }
            }
        } break;
        case 0x31: /* 0x31 - AND INDY */
            reg_a &= indy_get_src_data_page_cross();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x33: /* 0x33 - RLA INDY */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            src_addr = INDY_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;                   /* clear C flag */
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C flag */
            src_data <<= 1;
            src_data |= saved_c;           /* put the old C back at bit position 0 */
            cpu_write(src_addr, src_data); /* write it back to memory */
            reg_a &= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x35: /* 0x35 - AND ZPX */
        {
            uint8_t src_addr;

            src_addr = ZPX_GET_SRC_ADDR();
            cpu_read_zp(src_addr);
            reg_a &= cpu_read_zp(src_addr);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x36: /* 0x36 - ROL ZPX */
        {
            uint8_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;

            src_addr = ZPX_GET_SRC_ADDR();
            cpu_read_zp(src_addr);
            src_data = cpu_read_zp(src_addr);
            cpu_write_zp(src_addr, src_data); // dummy write
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;                   /* clear C flag */
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C flag */
            src_data <<= 1;
            src_data |= saved_c;              /* put the old C back at bit position 0 */
            cpu_write_zp(src_addr, src_data); /* write it back to memory */
            last_n_value = last_z_value = src_data;
        } break;
        case 0x37: /* 0x37 - RLA ZPX */
        {
            uint8_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 6 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 6 cycles total for the instruction
            src_addr = ZPX_GET_SRC_ADDR();
            src_data = cpu_read_zp(src_addr);
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;                   /* clear C flag */
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C flag */
            src_data <<= 1;
            src_data |= saved_c;              /* put the old C back at bit position 0 */
            cpu_write_zp(src_addr, src_data); /* write it back to memory */
            reg_a &= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x38: /* 0x38 - SEC IMP */
            IMP_DUMMY_FETCH();
            reg_p |= C_FLAG;
            break;
        case 0x39: /* 0x39 - AND ABSY */
            reg_a &= absy_get_src_data_page_cross();
            last_n_value = reg_a;
            last_z_value = reg_a;
            break;
        case 0x3B: /* 0x3B - RLA ABSY */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            src_addr = ABSY_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;                   /* clear C flag */
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C flag */
            src_data <<= 1;
            src_data |= saved_c;           /* put the old C back at bit position 0 */
            cpu_write(src_addr, src_data); /* write it back to memory */
            reg_a &= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x3D: /* 0x3D - AND ABSX */
            reg_a &= absx_get_src_data_page_cross();
            last_n_value = reg_a;
            last_z_value = reg_a;
            break;
        case 0x3E: /* 0x3E - ROL ABSX */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;

            src_addr = cpu_abs_indexed_dummy_read(reg_x);
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data); // dummy write
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;                   /* clear C flag */
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C flag */
            src_data <<= 1;
            src_data |= saved_c;           /* put the old C back at bit position 0 */
            cpu_write(src_addr, src_data); /* write it back to memory */
            last_n_value = last_z_value = src_data;
        } break;
        case 0x3F: /* 0x3F - RLA ABSX */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            src_addr = ABSX_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;                   /* clear C flag */
            reg_p |= ((src_data & BIT_7) >> 7); /* move MSB into C flag */
            src_data <<= 1;
            src_data |= saved_c;           /* put the old C back at bit position 0 */
            cpu_write(src_addr, src_data); /* write it back to memory */
            reg_a &= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x40:             /* 0x40 - RTI IMP */
            cpu_read_pc_inc(); // dummy read
            cpu_read_internal(0x100 + sp++);
            reg_p &= (BIT_4 | BIT_5);
            reg_p |= (cpu_read_internal(0x100 + sp++) & ~(BIT_4 | BIT_5));
            pc = cpu_read_internal(0x100 + sp++);
            pc |= (uint16_t)(cpu_read_internal(0x100 + sp) << 8);

            // N and Z are lazy: Z is set iff last_z_value == 0. (Previously 'last_z_value = reg_p' when Z was clear, which
            // made a pulled P of $00 read back with Z set -- instr_test-v5 14-rti/15-brk/16-special vs Mesen.)
            last_z_value = (reg_p & Z_FLAG) ? 0x00 : 0x01;
            last_n_value = reg_p;
            break;
        case 0x41: /* 0x41 - EOR INDX */
            cpu_read(pc);
            reg_a ^= INDX_GET_SRC_DATA();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x43: /* 0x43 - SRE INDX */
        {
            uint16_t src_addr;
            uint8_t src_data;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            src_addr = INDX_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            reg_p &= ~C_FLAG;
            reg_p |= (src_data & BIT_0); /* move LSB into C bit */
            src_data >>= 1;
            cpu_write(src_addr, src_data); /* write it back to memory */
            reg_a ^= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x44: /* 0x44 - NOP ZP (Undocumented) */ ZP_GET_SRC_DATA(); break;
        case 0x45: /* 0x45 - EOR ZP */
            reg_a ^= ZP_GET_SRC_DATA();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x46: /* 0x46 - LSR ZP */
        {
            uint8_t src_addr;
            uint8_t src_data;

            src_addr = ZP_GET_SRC_ADDR();
            src_data = cpu_read_zp(src_addr);
            cpu_write_zp(src_addr, src_data); // dummy write
            reg_p &= ~C_FLAG;
            reg_p |= (src_data & BIT_0); /* move LSB into C bit */
            src_data >>= 1;
            cpu_write_zp(src_addr, src_data); /* write it back to memory */
            last_n_value = last_z_value = src_data;
        } break;
        case 0x47: /* 0x47 - SRE ZP */
        {
            uint8_t src_addr;
            uint8_t src_data;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 5 cycles total for the instruction
            src_addr = ZP_GET_SRC_ADDR();
            src_data = cpu_read_zp(src_addr);
            reg_p &= ~C_FLAG;
            reg_p |= (src_data & BIT_0); /* move LSB into C bit */
            src_data >>= 1;
            cpu_write_zp(src_addr, src_data); /* write it back to memory */
            reg_a ^= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x48: /* 0x48 - PHA IMP */
            IMP_DUMMY_FETCH();
            PUSH_BYTE(reg_a);
            break;
        case 0x49: /* 0x49 - EOR IMM */
            reg_a ^= IMM_GET_SRC_DATA();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x4A: /* 0x4A - LSR ACC */
            cpu_read(pc);
            reg_p &= ~C_FLAG;
            reg_p |= (reg_a & BIT_0); /* move LSB into C bit */
            reg_a >>= 1;
            last_n_value = last_z_value = reg_a;
            break;
        case 0x4B: /* 0x4B - ASR IMM (Undocumented) */
            reg_a &= IMM_GET_SRC_DATA();
            reg_p &= ~C_FLAG;
            reg_p |= (reg_a & BIT_0); /* move LSB into C bit */
            reg_a >>= 1;
            last_n_value = last_z_value = reg_a;
            break;
        case 0x4C: /* 0x4C - JMP ABS */ pc = ABS_GET_SRC_ADDR(); break;
        case 0x4D: /* 0x4D - EOR ABS */
            reg_a ^= ABS_GET_SRC_DATA();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x4E: /* 0x4E - LSR ABS */
        {
            uint16_t src_addr;
            uint8_t src_data;

            src_addr = ABS_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data); // dummy write
            reg_p &= ~C_FLAG;
            reg_p |= (src_data & BIT_0); /* move LSB into C bit */
            src_data >>= 1;
            cpu_write(src_addr, src_data); /* write it back to memory */
            last_n_value = last_z_value = src_data;
        } break;
        case 0x4F: /* 0x4F - SRE ABS */
        {
            uint16_t src_addr;
            uint8_t src_data;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 6 cycles total for the instruction
            src_addr = ABS_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            reg_p &= ~C_FLAG;
            reg_p |= (src_data & BIT_0); /* move LSB into C bit */
            src_data >>= 1;
            cpu_write(src_addr, src_data); /* write it back to memory */
            reg_a ^= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x50: /* 0x50 - BVC REL */
        {
            int8_t branch_offset = (int8_t)cpu_read_pc_inc();
            // (the branch polls the NMI edge detector here: see nmi_poll_ready)
            if ((reg_p & V_FLAG) == 0) {
                uint16_t old_pc = pc;
                pc += branch_offset;
                cpu_branch_irq_delay(old_pc, pc);
                cpu_read(old_pc); // cycle 3: dummy read of the byte after the operand
                if ((pc ^ old_pc) & 0xFF00) {
                    // the high byte changed, indicates page crossing
                    // (the branch polls the NMI edge detector here: see nmi_poll_ready)
                    cpu_branch_irq_poll4();
                    cpu_read((old_pc & 0xFF00) | (pc & 0x00FF)); // cycle 4: dummy read before PCH is fixed
                }
            }
        } break;
        case 0x51: /* 0x51 - EOR INDY */
            reg_a ^= indy_get_src_data_page_cross();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x53: /* 0x53 - SRE INDY */
        {
            uint16_t src_addr;
            uint8_t src_data;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            src_addr = INDY_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            reg_p &= ~C_FLAG;
            reg_p |= (src_data & BIT_0); /* move LSB into C bit */
            src_data >>= 1;
            cpu_write(src_addr, src_data); /* write it back to memory */
            reg_a ^= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x55: /* 0x55 - EOR ZPX */
        {
            uint8_t src_addr;

            src_addr = ZPX_GET_SRC_ADDR();
            cpu_read_zp(src_addr);
            reg_a ^= cpu_read_zp(src_addr);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x56: /* 0x56 - LSR ZPX */
        {
            uint8_t src_addr;
            uint8_t src_data;

            src_addr = ZPX_GET_SRC_ADDR();
            cpu_read_zp(src_addr);
            src_data = cpu_read_zp(src_addr);
            cpu_write_zp(src_addr, src_data);
            reg_p &= ~C_FLAG;
            reg_p |= (src_data & BIT_0); /* move LSB into C bit */
            src_data >>= 1;
            cpu_write_zp(src_addr, src_data); /* write it back to memory */
            last_n_value = last_z_value = src_data;
        } break;
        case 0x57: /* 0x57 - SRE ZPX */
        {
            uint8_t src_addr;
            uint8_t src_data;

            src_addr = ZPX_GET_SRC_ADDR();
            cpu_read_zp(src_addr);
            src_data = cpu_read_zp(src_addr);
            cpu_write_zp(src_addr, src_data);
            reg_p &= ~C_FLAG;
            reg_p |= (src_data & BIT_0); /* move LSB into C bit */
            src_data >>= 1;
            cpu_write_zp(src_addr, src_data); /* write it back to memory */
            reg_a ^= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x58: /* 0x58 - CLI IMP */
            IMP_DUMMY_FETCH();
            reg_p &= ~I_FLAG; // takes effect now; the IRQ poll (irq_prev_run) was taken before this, hence the delay
            break;
        case 0x59: /* 0x59 - EOR ABSY */
            reg_a ^= absy_get_src_data_page_cross();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x5B: /* 0x5B - SRE ABSY */
        {
            uint16_t src_addr;
            uint8_t src_data;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            src_addr = ABSY_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            reg_p &= ~C_FLAG;
            reg_p |= (src_data & BIT_0); /* move LSB into C bit */
            src_data >>= 1;
            cpu_write(src_addr, src_data); /* write it back to memory */
            reg_a ^= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x5D: /* 0x5D - EOR ABSX */
            reg_a ^= absx_get_src_data_page_cross();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x5E: /* 0x5E - LSR ABSX */
        {
            uint8_t addr_low, addr_high;
            uint16_t old_addr;
            uint16_t src_addr;
            uint8_t src_data;

            addr_low = cpu_read_pc_inc();  // 2
            addr_high = cpu_read_pc_inc(); // 3
            src_addr = (((uint16_t)(addr_high) << 8) | addr_low) + reg_x;
            addr_low += reg_x;
            old_addr = ((uint16_t)(addr_high) << 8) | addr_low;

            cpu_read(old_addr);            //4
            src_data = cpu_read(src_addr); //5
            cpu_write(src_addr, src_data); // dummy write

            reg_p &= ~C_FLAG;
            reg_p |= (src_data & BIT_0); /* move LSB into C bit */
            src_data >>= 1;
            cpu_write(src_addr, src_data); /* write it back to memory */
            last_n_value = last_z_value = src_data;
        } break;
        case 0x5F: /* 0x5F - SRE ABSX */
        {
            uint16_t src_addr;
            uint8_t src_data;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            src_addr = ABSX_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            reg_p &= ~C_FLAG;
            reg_p |= (src_data & BIT_0); /* move LSB into C bit */
            src_data >>= 1;
            cpu_write(src_addr, src_data); /* write it back to memory */
            reg_a ^= src_data;
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x60:                         /* 0x60 - RTS IMP */
            cpu_read(pc);                  // dummy
            cpu_read_internal(0x100 + sp); // dummy read?
            pc = (uint16_t)cpu_read_internal(++sp + 0x100);
            pc |= ((uint16_t)cpu_read_internal(++sp + 0x100) << 8);
            cpu_read_pc_inc(); // increment PC
            break;
        case 0x61: /* 0x61 - ADC INDX */
        {
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            cpu_read(pc);
            src_data = INDX_GET_SRC_DATA();
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x63: /* 0x63 - RRA INDX */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;
            uint8_t dest_data;
            uint8_t temp_flag;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            src_addr = INDX_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data); // dummy write
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;            /* clear C flag */
            reg_p |= (src_data & BIT_0); /* move LSB into C flag */
            src_data >>= 1;
            src_data |= (saved_c << 7); /* factor old C bit back in */
            cpu_write(src_addr, src_data);
            // now ADC
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x64: /* 0x64 - NOP ZP (Undocumented) */ ZP_GET_SRC_DATA(); break;
        case 0x65: /* 0x65 - ADC ZP */
        {
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            src_data = ZP_GET_SRC_DATA();
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x66: /* 0x66 - ROR ZP */
        {
            uint8_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;

            src_addr = ZP_GET_SRC_ADDR();
            src_data = cpu_read_zp(src_addr);
            cpu_write_zp(src_addr, src_data); // dummy write
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;            /* clear C flag */
            reg_p |= (src_data & BIT_0); /* move LSB into C flag */
            src_data >>= 1;
            src_data |= (saved_c << 7); /* factor old C bit back in */
            cpu_write_zp(src_addr, src_data);
            last_n_value = last_z_value = src_data;
        } break;
        case 0x67: /* 0x67 - RRA ZP */
        {
            uint8_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;
            uint8_t dest_data;
            uint8_t temp_flag;

            src_addr = ZP_GET_SRC_ADDR();
            src_data = cpu_read_zp(src_addr);
            cpu_write(src_addr, src_data); // dummy write
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;            /* clear C flag */
            reg_p |= (src_data & BIT_0); /* move LSB into C flag */
            src_data >>= 1;
            src_data |= (saved_c << 7); /* factor old C bit back in */
            cpu_write_zp(src_addr, src_data);
            // now ADC
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x68: /* 0x68 - PLA IMP */
            IMP_DUMMY_FETCH();
            cpu_read_internal(sp + 0x100); // dummy read
            reg_a = POP_BYTE();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x69: /* 0x69 - ADC IMM */
        {
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            src_data = IMM_GET_SRC_DATA();
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x6A: /* 0x6A - ROR ACC */
        {
            uint8_t saved_c;

            cpu_read(pc);
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;         /* clear C flag */
            reg_p |= (reg_a & BIT_0); /* move LSB into C flag */
            reg_a >>= 1;
            reg_a |= (saved_c << 7); /* factor old C bit back in */
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x6B: /* 0x6B - ARR IMM (Undocumented) */
        {
            uint8_t saved_c;

            saved_c = (reg_p & C_FLAG);
            reg_a &= IMM_GET_SRC_DATA();
            reg_a >>= 1;
            reg_a |= (saved_c << 7); /* factor old C bit back in */

            reg_p &= ~(C_FLAG | V_FLAG);               /* clear C and V flags */
            reg_p |= ((reg_a >> 6) & BIT_0);           // C is bit 6
            reg_p |= ((reg_a ^ (reg_a << 1)) & BIT_6); // V is bit 6 xor bit 5.
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x6C: /* 0x6C - JMP IND */
        {
            uint16_t src_addr;
            uint8_t addr_low;
            uint8_t addr_high;
            addr_low = cpu_read_pc_inc();
            addr_high = cpu_read_pc_inc();
            src_addr = ((uint16_t)addr_low | (uint16_t)(addr_high << 8));
            pc = cpu_read(src_addr);
            addr_low++;
            src_addr = ((uint16_t)addr_low | (uint16_t)(addr_high << 8));
            pc |= (cpu_read(src_addr) << 8);
        } break;
        case 0x6D: /* 0x6D - ADC ABS */
        {
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            src_data = ABS_GET_SRC_DATA();
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x6E: /* 0x6E - ROR ABS */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;

            src_addr = ABS_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data); // dummy write
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;            /* clear C flag */
            reg_p |= (src_data & BIT_0); /* move LSB into C flag */
            src_data >>= 1;
            src_data |= (saved_c << 7); /* factor old C bit back in */
            cpu_write(src_addr, src_data);
            last_n_value = last_z_value = src_data;
        } break;
        case 0x6F: /* 0x6F - RRA ABS */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;
            uint8_t dest_data;
            uint8_t temp_flag;

            src_addr = ABS_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data); // dummy write
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;            /* clear C flag */
            reg_p |= (src_data & BIT_0); /* move LSB into C flag */
            src_data >>= 1;
            src_data |= (saved_c << 7); /* factor old C bit back in */
            cpu_write(src_addr, src_data);
            // now ADC
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x70: /* 0x70 - BVS REL */
        {
            int8_t branch_offset = (int8_t)cpu_read_pc_inc();
            // (the branch polls the NMI edge detector here: see nmi_poll_ready)
            if (reg_p & V_FLAG) {
                uint16_t old_pc = pc;
                pc += branch_offset;
                cpu_branch_irq_delay(old_pc, pc);
                cpu_read(old_pc); // cycle 3: dummy read of the byte after the operand
                if ((pc ^ old_pc) & 0xFF00) {
                    // the high byte changed, indicates page crossing
                    // (the branch polls the NMI edge detector here: see nmi_poll_ready)
                    cpu_branch_irq_poll4();
                    cpu_read((old_pc & 0xFF00) | (pc & 0x00FF)); // cycle 4: dummy read before PCH is fixed
                }
            }
        } break;
        case 0x71: /* 0x71 - ADC INDY */
        {
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            src_data = indy_get_src_data_page_cross();
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x73: /* 0x73 - RRA INDY */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;
            uint8_t dest_data;
            uint8_t temp_flag;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            src_addr = INDY_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data); // dummy write
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;            /* clear C flag */
            reg_p |= (src_data & BIT_0); /* move LSB into C flag */
            src_data >>= 1;
            src_data |= (saved_c << 7); /* factor old C bit back in */
            cpu_write(src_addr, src_data);
            // now ADC
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x75: /* 0x75 - ADC ZPX */
        {
            uint8_t src_addr;
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;

            src_addr = ZPX_GET_SRC_ADDR();
            cpu_read_zp(src_addr);
            src_data = cpu_read_zp(src_addr);
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x76: /* 0x76 - ROR ZPX */
        {
            uint8_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;

            src_addr = ZPX_GET_SRC_ADDR();
            cpu_read_zp(src_addr);
            src_data = cpu_read_zp(src_addr);
            cpu_write_zp(src_addr, src_data); // dummy write
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;            /* clear C flag */
            reg_p |= (src_data & BIT_0); /* move LSB into C flag */
            src_data >>= 1;
            src_data |= (saved_c << 7); /* factor old C bit back in */
            cpu_write_zp(src_addr, src_data);
            last_n_value = last_z_value = src_data;
        } break;
        case 0x77: /* 0x77 - RRA ZPX */
        {
            uint8_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;
            uint8_t dest_data;
            uint8_t temp_flag;

            src_addr = ZPX_GET_SRC_ADDR();
            cpu_read_zp(src_addr);
            src_data = cpu_read_zp(src_addr);
            cpu_write(src_addr, src_data); // dummy write
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;            /* clear C flag */
            reg_p |= (src_data & BIT_0); /* move LSB into C flag */
            src_data >>= 1;
            src_data |= (saved_c << 7); /* factor old C bit back in */
            cpu_write_zp(src_addr, src_data);
            // now ADC
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x78: /* 0x78 - SEI IMP */
            IMP_DUMMY_FETCH();
            reg_p |= I_FLAG; // takes effect now; the IRQ poll (irq_prev_run) was taken before this, hence the delay
            break;
        case 0x79: /* 0x79 - ADC ABSY */
        {
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            src_data = absy_get_src_data_page_cross();
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x7B: /* 0x7B - RRA ABSY */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;
            uint8_t dest_data;
            uint8_t temp_flag;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            src_addr = ABSY_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data); // dummy write
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;            /* clear C flag */
            reg_p |= (src_data & BIT_0); /* move LSB into C flag */
            src_data >>= 1;
            src_data |= (saved_c << 7); /* factor old C bit back in */
            cpu_write(src_addr, src_data);
            // now ADC
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x7D: /* 0x7D - ADC ABSX */
        {
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            src_data = absx_get_src_data_page_cross();
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x7E: /* 0x7E - ROR ABSX */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;

            src_addr = cpu_abs_indexed_dummy_read(reg_x);
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data); // dummy write
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;            /* clear C flag */
            reg_p |= (src_data & BIT_0); /* move LSB into C flag */
            src_data >>= 1;
            src_data |= (saved_c << 7); /* factor old C bit back in */
            cpu_write(src_addr, src_data);
            last_n_value = last_z_value = src_data;
        } break;
        case 0x7F: /* 0x7F - RRA ABSX */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t saved_c;
            uint8_t dest_data;
            uint8_t temp_flag;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            src_addr = ABSX_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data); // dummy write
            saved_c = (reg_p & C_FLAG);
            reg_p &= ~C_FLAG;            /* clear C flag */
            reg_p |= (src_data & BIT_0); /* move LSB into C flag */
            src_data >>= 1;
            src_data |= (saved_c << 7); /* factor old C bit back in */
            cpu_write(src_addr, src_data);
            // now ADC
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x80: /* 0x80 - NOP IMM (Undocumented) */
        case 0x82: /* 0x82 - NOP IMM (Undocumented) */
        case 0x89: /* 0x89 - NOP IMM (Undocumented) */
        case 0xC2: /* 0xC2 - NOP IMM (Undocumented) */
        case 0xE2: /* 0xE2 - NOP IMM (Undocumented) */ cpu_read_pc_inc(); break;
        case 0x81: /* 0x81 - STA INDX */
            cpu_read(pc);
            cpu_write(INDX_GET_SRC_ADDR(), reg_a);
            break;
        case 0x83: /* 0x83 - SAX INDX (Undocumented) */
            cpu_read(pc);
            cpu_write(INDX_GET_SRC_ADDR(), reg_a & reg_x);
            break;
        case 0x84: /* 0x84 - STY ZP */ cpu_write_zp(ZP_GET_SRC_ADDR(), reg_y); break;
        case 0x85: /* 0x85 - STA ZP */ cpu_write_zp(ZP_GET_SRC_ADDR(), reg_a); break;
        case 0x86: /* 0x86 - STX ZP */ cpu_write_zp(ZP_GET_SRC_ADDR(), reg_x); break;
        case 0x87: /* 0x87 - SAX ZP (Undocumented) */ cpu_write_zp(ZP_GET_SRC_ADDR(), reg_a & reg_x); break;
        case 0x88: /* 0x88 - DEY IMP */
            IMP_DUMMY_FETCH();
            reg_y--;
            last_n_value = last_z_value = reg_y;
            break;
        case 0x8A: /* 0x8A - TXA IMP */
            IMP_DUMMY_FETCH();
            reg_a = reg_x;
            last_n_value = last_z_value = reg_a;
            break;
        case 0x8C: /* 0x8C - STY ABS */ cpu_write(ABS_GET_SRC_ADDR(), reg_y); break;
        case 0x8B: /* 0x8B - XAA IMM */ // see http://visual6502.org/wiki/index.php%3Ftitle%3D6502_Opcode_8B_(XAA,_ANE)
            reg_a = (reg_a | 0xFF) & reg_x & IMM_GET_SRC_DATA();
            last_n_value = last_z_value = reg_a;
            break;
        case 0x8D: /* 0x8D - STA ABS */ cpu_write(ABS_GET_SRC_ADDR(), reg_a); break;
        case 0x8E: /* 0x8E - STX ABS */ cpu_write(ABS_GET_SRC_ADDR(), reg_x); break;
        case 0x8F: /* 0x8F - SAX ABS (Undocumented) */ cpu_write(ABS_GET_SRC_ADDR(), reg_a & reg_x); break;
        case 0x90: /* 0x90 - BCC REL */
        {
            int8_t branch_offset = (int8_t)cpu_read_pc_inc();
            // (the branch polls the NMI edge detector here: see nmi_poll_ready)
            if ((reg_p & C_FLAG) == 0) {
                uint16_t old_pc = pc;
                pc += branch_offset;
                cpu_branch_irq_delay(old_pc, pc);
                cpu_read(old_pc); // cycle 3: dummy read of the byte after the operand
                if ((pc ^ old_pc) & 0xFF00) {
                    // the high byte changed, indicates page crossing
                    // (the branch polls the NMI edge detector here: see nmi_poll_ready)
                    cpu_branch_irq_poll4();
                    cpu_read((old_pc & 0xFF00) | (pc & 0x00FF)); // cycle 4: dummy read before PCH is fixed
                }
            }
        } break;
        case 0x91: /* 0x91 - STA INDY */
        {
            uint16_t address = cpu_read_word_zp(cpu_read_pc_inc());
            uint16_t dummy_address = address & 0xff00;
            address += reg_y;
            dummy_address += (address & 0xff);
            cpu_read(dummy_address);
            cpu_write(address, reg_a);
        } break;

        case 0x93: /* 0x93 - SHA (AHX) INDY */
        {
            uint8_t zp = cpu_read_pc_inc();
            cpu_unstable_store(cpu_read_word_zp(zp), reg_y, reg_a & reg_x);
        } break;

        case 0x94: /* 0x94 - STY ZPX */
        {
            uint8_t zp_addr = ZPX_GET_SRC_ADDR();
            cpu_read_zp(zp_addr);
            cpu_write_zp(zp_addr, reg_y);
        } break;
        case 0x95: /* 0x95 - STA ZPX */
        {
            uint8_t address = ZPX_GET_SRC_ADDR();
            cpu_read_zp(address);
            cpu_write_zp(address, reg_a);
        } break;
        case 0x96: /* 0x96 - STX ZPY */
        {
            uint8_t address = ZPY_GET_SRC_ADDR();
            cpu_read_zp(address);
            cpu_write_zp(address, reg_x);
        } break;
        case 0x97: /* 0x97 - SAX ZPY (Undocumented) */
        {
            uint8_t address = ZPY_GET_SRC_ADDR();
            cpu_read_zp(address);
            cpu_write_zp(address, reg_a & reg_x);
        } break;
        case 0x98: /* 0x98 - TYA IMP */
            IMP_DUMMY_FETCH();
            reg_a = reg_y;
            last_n_value = last_z_value = reg_a;
            break;
        case 0x99: /* 0x99 - STA ABSY */
        {
            uint16_t addr = cpu_abs_indexed_dummy_read(reg_y);
            cpu_write(addr, reg_a);
        } break;
        case 0x9A: /* 0x9A - TXS IMP */
            IMP_DUMMY_FETCH();
            sp = reg_x;
            break;
        case 0x9B: /* TAS a,y  also called XAS (5 cycles) */
        {
            uint16_t base = cpu_read_abs_operand();
            sp = reg_a & reg_x;
            cpu_unstable_store(base, reg_y, sp);
        } break;
        case 0x9C: /* 0x9C - SHY ABSX */ cpu_unstable_store(cpu_read_abs_operand(), reg_x, reg_y); break;
        case 0x9D: /* 0x9D - STA ABSX */
        {
            uint16_t addr = ABS_GET_SRC_ADDR();
            uint16_t dummy_addr = addr & 0xff00;
            addr += reg_x;
            dummy_addr += (addr & 0xff);
            cpu_read(dummy_addr); // dummy read
            cpu_write(addr, reg_a);
        } break;
        case 0x9E: /* 0x9E - SHX ABSY */ cpu_unstable_store(cpu_read_abs_operand(), reg_y, reg_x); break;
        case 0x9F: /* 0x9F - SHA (AHX) ABSY (also AXA) */
            cpu_unstable_store(cpu_read_abs_operand(), reg_y, reg_a & reg_x);
            break;
        case 0xA0: /* 0xA0 - LDY IMM */
            reg_y = IMM_GET_SRC_DATA();
            last_n_value = last_z_value = reg_y;
            break;
        case 0xA1: /* 0xA1 - LDA INDX */
        {
            uint8_t zp_addr;
            uint16_t addr;
            zp_addr = cpu_read_pc_inc(); // get pointer address
            cpu_read_zp(zp_addr);        // dummy
            addr = cpu_read_word_zp(zp_addr + reg_x);
            reg_a = cpu_read(addr);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xA2: /* 0xA2 - LDX IMM */
            reg_x = IMM_GET_SRC_DATA();
            last_n_value = last_z_value = reg_x;
            break;
        case 0xA3: /* 0xA3 - LAX INDX (Undocumented) */
        {
            uint8_t zp_addr;
            uint16_t addr;
            zp_addr = cpu_read_pc_inc(); // get pointer address
            cpu_read_zp(zp_addr);        // dummy
            addr = cpu_read_word_zp(zp_addr + reg_x);
            reg_a = reg_x = cpu_read(addr);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xA4: /* 0xA4 - LDY ZP */
            reg_y = ZP_GET_SRC_DATA();
            last_n_value = last_z_value = reg_y;
            break;
        case 0xA5: /* 0xA5 - LDA ZP */
            reg_a = ZP_GET_SRC_DATA();
            last_n_value = last_z_value = reg_a;
            break;
        case 0xA6: /* 0xA6 - LDX ZP */
            reg_x = ZP_GET_SRC_DATA();
            last_n_value = last_z_value = reg_x;
            break;
        case 0xA7: /* 0xA7 - LAX ZP (Undocumented) */
            reg_x = reg_a = ZP_GET_SRC_DATA();
            last_n_value = last_z_value = reg_x;
            break;
        case 0xA8: /* 0xA8 - TAY IMP */
            IMP_DUMMY_FETCH();
            reg_y = reg_a;
            last_n_value = last_z_value = reg_y;
            break;
        case 0xA9: /* 0xA9 - LDA IMM */
            reg_a = IMM_GET_SRC_DATA();
            last_n_value = last_z_value = reg_a;
            break;
        case 0xAA: /* 0xAA - TAX IMP */
            IMP_DUMMY_FETCH();
            reg_x = reg_a;
            last_n_value = last_z_value = reg_x;
            break;
        case 0xAB: /* 0xAB - LAX IMM (Undocumented) */
            reg_a = IMM_GET_SRC_DATA();
            reg_x = reg_a;
            last_n_value = last_z_value = reg_x;
            break;
        case 0xAC: /* 0xAC - LDY ABS */
            reg_y = ABS_GET_SRC_DATA();
            last_n_value = last_z_value = reg_y;
            break;
        case 0xAD: /* 0xAD - LDA ABS */
            reg_a = ABS_GET_SRC_DATA();
            last_n_value = last_z_value = reg_a;
            break;
        case 0xAE: /* 0xAE - LDX ABS */
            reg_x = ABS_GET_SRC_DATA();
            last_n_value = last_z_value = reg_x;
            break;
        case 0xAF: /* 0xAF - LAX ABS (Undocumented) */
            reg_x = reg_a = ABS_GET_SRC_DATA();
            last_n_value = last_z_value = reg_x;
            break;
        case 0xB0: /* 0xB0 - BCS REL */
        {
            int8_t branch_offset = (int8_t)cpu_read_pc_inc();
            // (the branch polls the NMI edge detector here: see nmi_poll_ready)
            if (reg_p & C_FLAG) {
                uint16_t old_pc = pc;
                pc += branch_offset;
                cpu_branch_irq_delay(old_pc, pc);
                cpu_read(old_pc); // cycle 3: dummy read of the byte after the operand
                if ((pc ^ old_pc) & 0xFF00) {
                    // the high byte changed, indicates page crossing
                    // (the branch polls the NMI edge detector here: see nmi_poll_ready)
                    cpu_branch_irq_poll4();
                    cpu_read((old_pc & 0xFF00) | (pc & 0x00FF)); // cycle 4: dummy read before PCH is fixed
                }
            }
        } break;
        case 0xB1: /* 0xB1 - LDA INDY */
        {
            uint8_t zp_addr;
            uint16_t addr;
            uint16_t old_addr;

            zp_addr = cpu_read_pc_inc();
            old_addr = addr = cpu_read_word_zp(zp_addr);
            addr += reg_y;
            reg_a = cpu_read((old_addr & 0xFF00) | (addr & 0x00FF));
            if ((addr ^ old_addr) & 0xFF00) {
                // the high byte changed, indicates page crossing
                reg_a = cpu_read(addr);
            }
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xB3: /* 0xB3 - LAX INDY (Undocumented) */
        {
            uint8_t zp_addr;
            uint16_t addr;
            uint16_t old_addr;

            zp_addr = cpu_read_pc_inc();
            old_addr = addr = cpu_read_word_zp(zp_addr);
            addr += reg_y;
            reg_a = reg_x = cpu_read((old_addr & 0xFF00) | (addr & 0x00FF));
            if ((addr ^ old_addr) & 0xFF00) {
                // the high byte changed, indicates page crossing
                reg_a = reg_x = cpu_read(addr);
            }
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xB4: /* 0xB4 - LDY ZPX */
        {
            uint8_t zp_addr = ZPX_GET_SRC_ADDR();
            cpu_read_zp(zp_addr);
            reg_y = cpu_read_zp(zp_addr);
            last_n_value = last_z_value = reg_y;
        } break;
        case 0xB5: /* 0xB5 - LDA ZPX */
        {
            uint8_t src_addr;
            src_addr = cpu_read_pc_inc();
            cpu_read(src_addr); // dummy read
            src_addr = (src_addr + reg_x) & 0xFF;
            reg_a = cpu_read_zp(src_addr);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xB6: /* 0xB6 - LDX ZPY */
        {
            uint8_t src_addr;
            src_addr = ZPY_GET_SRC_ADDR();
            cpu_read(src_addr); // dummy read
            reg_x = cpu_read_zp(src_addr);
            last_n_value = last_z_value = reg_x;
        } break;
        case 0xB7: /* 0xB7 - LAX ZPY (Undocumented) */
        {
            uint8_t src_addr;
            src_addr = ZPY_GET_SRC_ADDR();
            cpu_read(src_addr); // dummy read
            reg_x = reg_a = cpu_read_zp(src_addr);
            last_n_value = last_z_value = reg_x;
        } break;
        case 0xB8: /* 0xB8 - CLV IMP */
            IMP_DUMMY_FETCH();
            reg_p &= ~V_FLAG;
            break;
        case 0xB9: /* 0xB9 - LDA ABSY */
        {
            uint8_t addr_low;
            uint8_t addr_high;
            uint16_t addr;

            addr_low = cpu_read_pc_inc();
            addr_high = cpu_read_pc_inc();
            addr = ((addr_high << 8) | addr_low) + reg_y;
            addr_low += reg_y;
            reg_a = cpu_read((addr_high << 8) | addr_low);
            if ((addr >> 8) != addr_high) {
                reg_a = cpu_read(addr);
            }
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xBA: /* 0xBA - TSX IMP */
            IMP_DUMMY_FETCH();
            reg_x = sp;
            last_n_value = last_z_value = reg_x;
            break;
        case 0xBB: /* 0xBB - LAS ABSY */
            sp &= absy_get_src_data_page_cross();
            reg_a = sp;
            reg_x = sp;
            last_n_value = last_z_value = reg_a;
            break;
        case 0xBC: /* 0xBC - LDY ABSX */
        {
            uint16_t addr;
            uint16_t addr_old;
            uint8_t addr_low;
            uint8_t addr_high;
            addr_low = cpu_read_pc_inc();
            addr_high = cpu_read_pc_inc();
            addr = ((uint16_t)addr_low | (uint16_t)(addr_high << 8)) + reg_x;
            addr_low += reg_x;
            addr_old = (uint16_t)addr_low | ((uint16_t)(addr_high) << 8);
            reg_y = cpu_read(addr_old);
            if (addr_old != addr) {
                reg_y = cpu_read(addr);
            }
            last_n_value = last_z_value = reg_y;
        } break;
        case 0xBD: /* 0xBD - LDA ABSX */
        {
            uint16_t addr;
            uint16_t addr_old;
            uint8_t addr_low;
            uint8_t addr_high;
            addr_low = cpu_read_pc_inc();
            addr_high = cpu_read_pc_inc();
            addr = ((uint16_t)addr_low | (uint16_t)(addr_high << 8)) + reg_x;
            addr_low += reg_x;
            addr_old = (uint16_t)addr_low | ((uint16_t)(addr_high) << 8);
            reg_a = cpu_read(addr_old);
            if (addr_old != addr) {
                reg_a = cpu_read(addr);
            }
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xBE: /* 0xBE - LDX ABSY */
        {
            uint16_t addr;
            uint16_t addr_old;
            uint8_t addr_low;
            uint8_t addr_high;
            addr_low = cpu_read_pc_inc();
            addr_high = cpu_read_pc_inc();
            addr = ((uint16_t)addr_low | (uint16_t)(addr_high << 8)) + reg_y;
            addr_low += reg_y;
            addr_old = (uint16_t)addr_low | ((uint16_t)(addr_high) << 8);
            reg_x = cpu_read(addr_old);
            if (addr_old != addr) {
                reg_x = cpu_read(addr);
            }
            last_n_value = last_z_value = reg_x;
        } break;
        case 0xBF: /* 0xBF - LAX ABSY (Undocumented) */
            reg_x = reg_a = absy_get_src_data_page_cross();
            last_n_value = last_z_value = reg_x;
            break;
        case 0xC0: /* 0xC0 - CPY IMM */
        {
            uint8_t src_data;
            uint8_t dest_data;

            src_data = IMM_GET_SRC_DATA();
            dest_data = reg_y - src_data;
            if (reg_y >= src_data) {
                reg_p |= C_FLAG;
            } else {
                reg_p &= ~C_FLAG;
            }
            last_n_value = last_z_value = dest_data;
        } break;
        case 0xC1: /* 0xC1 - CMP INDX */
        {
            uint8_t src_data;
            uint8_t dest_data;

            cpu_read(pc);
            src_data = INDX_GET_SRC_DATA();
            dest_data = reg_a - src_data;
            if (reg_a >= src_data) {
                reg_p |= C_FLAG;
            } else {
                reg_p &= ~C_FLAG;
            }
            last_n_value = last_z_value = dest_data;
        } break;
        case 0xC3: /* 0xC3 - DCP INDX (Undocumented) */
        {
            uint8_t zp_addr;
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t dest_data;

            zp_addr = cpu_read_pc_inc() + reg_x;
            cpu_read_zp(zp_addr);
            src_addr = cpu_read_word_zp(zp_addr);
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data); // dummy
            src_data--;                    /* decrement */
            dest_data = reg_a - src_data;
            if (reg_a >= src_data) {
                reg_p |= C_FLAG;
            } else {
                reg_p &= ~C_FLAG;
            }
            cpu_write(src_addr, src_data);
            last_n_value = last_z_value = dest_data;
        } break;
        case 0xC4: /* 0xC4 - CPY ZP */
        {
            uint8_t src_data;
            uint8_t dest_data;

            src_data = ZP_GET_SRC_DATA();
            dest_data = reg_y - src_data;
            if (reg_y >= src_data) {
                reg_p |= C_FLAG;
            } else {
                reg_p &= ~C_FLAG;
            }
            last_n_value = last_z_value = dest_data;
        } break;
        case 0xC5: /* 0xC5 - CMP ZP */
        {
            uint8_t src_data;
            uint8_t dest_data;

            src_data = ZP_GET_SRC_DATA();
            dest_data = reg_a - src_data;
            if (reg_a >= src_data) {
                reg_p |= C_FLAG;
            } else {
                reg_p &= ~C_FLAG;
            }
            last_n_value = last_z_value = dest_data;
        } break;
        case 0xC6: /* 0xC6 - DEC ZP */
        {
            uint8_t src_addr;
            uint8_t src_data;

            src_addr = ZP_GET_SRC_ADDR();
            src_data = cpu_read_zp(src_addr);
            cpu_write_zp(src_addr, src_data); // dummy write
            src_data--;
            cpu_write_zp(src_addr, src_data); // dummy write
            last_n_value = last_z_value = src_data;
        } break;
        case 0xC7: /* 0xC7 - DCP ZP */
        {
            uint8_t src_addr;
            uint8_t src_data;
            uint8_t dest_data;

            src_addr = ZP_GET_SRC_ADDR();
            src_data = cpu_read_zp(src_addr);
            cpu_write_zp(src_addr, src_data); // dummy write
            src_data--;
            dest_data = reg_a - src_data;
            if (reg_a >= src_data) {
                reg_p |= C_FLAG;
            } else {
                reg_p &= ~C_FLAG;
            }
            cpu_write_zp(src_addr, src_data);
            last_n_value = last_z_value = dest_data;
        } break;
        case 0xC8: /* 0xC8 - INY IMP */
            IMP_DUMMY_FETCH();
            reg_y++;
            last_n_value = last_z_value = reg_y;
            break;
        case 0xC9: /* 0xC9 - CMP IMM */
        {
            uint8_t src_data;
            uint8_t dest_data;

            src_data = IMM_GET_SRC_DATA();
            dest_data = reg_a - src_data;
            if (reg_a >= src_data) {
                reg_p |= C_FLAG;
            } else {
                reg_p &= ~C_FLAG;
            }
            last_n_value = last_z_value = dest_data;
        } break;
        case 0xCA: /* 0xCA - DEX IMP */
            IMP_DUMMY_FETCH();
            reg_x--;
            last_n_value = last_z_value = reg_x;
            break;
        case 0xCB: /* 0xCB - AXS IMM (Undocumented) */
        {
            uint8_t src_data;

            src_data = IMM_GET_SRC_DATA();
            reg_x &= reg_a;
            reg_p &= ~C_FLAG; /* clear C flag */
            if (reg_x >= src_data) {
                reg_p |= C_FLAG;
            }
            reg_x -= src_data;
            last_n_value = last_z_value = reg_x;
        } break;
        case 0xCC: /* 0xCC - CPY ABS */
        {
            uint8_t src_data;
            uint8_t dest_data;

            src_data = ABS_GET_SRC_DATA();
            dest_data = reg_y - src_data;
            if (reg_y >= src_data) {
                reg_p |= C_FLAG;
            } else {
                reg_p &= ~C_FLAG;
            }
            last_n_value = last_z_value = dest_data;
        } break;
        case 0xCD: /* 0xCD - CMP ABS */
        {
            uint16_t src_addr;
            uint8_t src_data;

            src_addr = ABS_GET_SRC_ADDR(); // cycles 2, 3
            src_data = cpu_read(src_addr); // cycle 4
            reg_p &= ~C_FLAG;
            if (reg_a >= src_data) {
                reg_p |= C_FLAG;
            }
            last_n_value = last_z_value = reg_a - src_data;
        } break;
        case 0xCE: /* 0xCE - DEC ABS */
        {
            uint16_t src_addr;
            uint8_t src_data;
            src_addr = ABS_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data); // dummy write
            src_data--;
            cpu_write(src_addr, src_data);
            last_n_value = last_z_value = src_data;
            break;
        }
        case 0xCF: /* 0xCF - DCP ABS (Undocumented)*/
        {
            uint16_t src_addr;
            uint8_t src_data;
            src_addr = ABS_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data);
            src_data--;
            reg_p &= ~C_FLAG;
            if (reg_a >= src_data) {
                reg_p |= C_FLAG;
            }
            cpu_write(src_addr, src_data);
            last_n_value = last_z_value = reg_a - src_data;
            break;
        }
        case 0xD0: /* 0xD0 - BNE REL */
        {
            int8_t branch_offset = (int8_t)cpu_read_pc_inc();
            // (the branch polls the NMI edge detector here: see nmi_poll_ready)
            if (last_z_value != 0) {
                uint16_t old_pc = pc;
                pc += branch_offset;
                cpu_branch_irq_delay(old_pc, pc);
                cpu_read(old_pc); // cycle 3: dummy read of the byte after the operand
                if ((pc ^ old_pc) & 0xFF00) {
                    // the high byte changed, indicates page crossing
                    // (the branch polls the NMI edge detector here: see nmi_poll_ready)
                    cpu_branch_irq_poll4();
                    cpu_read((old_pc & 0xFF00) | (pc & 0x00FF)); // cycle 4: dummy read before PCH is fixed
                }
            }
        } break;
        case 0xD1: /* 0xD1 - CMP INDY */
        {
            uint8_t src_data;
            uint8_t dest_data;

            src_data = indy_get_src_data_page_cross();
            dest_data = reg_a - src_data;
            if (reg_a >= src_data) {
                reg_p |= C_FLAG;
            } else {
                reg_p &= ~C_FLAG;
            }
            last_n_value = last_z_value = dest_data;
        } break;
        case 0xD3: /* 0xD3 - DCP INDY (Undocumented)*/
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t dest_data;

            cpu_add_cycles(CPU_ONE_CYCLE);  // pad to get 8 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE);  // pad to get 8 cycles total for the instruction
            src_addr = INDY_GET_SRC_ADDR(); // 2 cpu cycles
            src_data = cpu_read(src_addr);  // 1 cpu cycles
            src_data--;
            dest_data = reg_a - src_data;
            if (reg_a >= src_data) {
                reg_p |= C_FLAG;
            } else {
                reg_p &= ~C_FLAG;
            }
            cpu_write(src_addr, src_data); // 1 cpu cycle
            last_n_value = last_z_value = dest_data;
        } break;
        case 0xD5: /* 0xD5 - CMP ZPX */
        {
            uint8_t src_addr;
            uint8_t src_data;
            uint8_t dest_data;

            src_addr = ZPX_GET_SRC_ADDR();
            cpu_read_zp(src_addr);
            src_data = cpu_read_zp(src_addr);
            dest_data = reg_a - src_data;
            if (reg_a >= src_data) {
                reg_p |= C_FLAG;
            } else {
                reg_p &= ~C_FLAG;
            }
            last_n_value = last_z_value = dest_data;
        } break;
        case 0xD6: /* 0xD6 - DEC ZPX */
        {
            uint8_t src_addr;
            uint8_t src_data;

            src_addr = ZPX_GET_SRC_ADDR();
            cpu_read_zp(src_addr);
            src_data = cpu_read_zp(src_addr);
            cpu_write_zp(src_addr, src_data); // dummy write
            src_data--;
            cpu_write_zp(src_addr, src_data); // dummy write
            last_n_value = last_z_value = src_data;
        } break;
        case 0xD7: /* 0xD7 - DCP ZPX (Undocumented)*/
        {
            uint8_t src_addr;
            uint8_t src_data;
            uint8_t dest_data;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            src_addr = ZPX_GET_SRC_ADDR();
            src_data = cpu_read_zp(src_addr);
            cpu_write_zp(src_addr, src_data); // dummy write
            src_data--;
            dest_data = reg_a - src_data;
            if (reg_a >= src_data) {
                reg_p |= C_FLAG;
            } else {
                reg_p &= ~C_FLAG;
            }
            cpu_write_zp(src_addr, src_data);
            last_n_value = last_z_value = dest_data;

        } break;
        case 0xD8: /* 0xD8 - CLD IMP */
            IMP_DUMMY_FETCH();
            reg_p &= ~D_FLAG;
            break;
        case 0xD9: /* 0xD9 - CMP ABSY */
        {
            uint8_t src_data;

            src_data = absy_get_src_data_page_cross();
            reg_p &= ~C_FLAG;
            if (reg_a >= src_data) {
                reg_p |= C_FLAG;
            }
            last_n_value = last_z_value = reg_a - src_data;
        } break;
        case 0xDB: /* 0xDB - DCP ABSY (Undocumented) */
        {
            uint16_t src_addr;
            uint8_t src_data;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            src_addr = ABS_GET_SRC_ADDR(); // cycles 2, 3
            src_addr += reg_y;
            src_data = cpu_read(src_addr); // cycle 4
            src_data--;

            reg_p &= ~C_FLAG;
            if (reg_a >= src_data) {
                reg_p |= C_FLAG;
            }
            cpu_write(src_addr, src_data);
            last_n_value = last_z_value = reg_a - src_data;
        } break;
        case 0xDD: /* 0xDD - CMP ABSX */
        {
            uint8_t src_data;

            src_data = absx_get_src_data_page_cross();
            reg_p &= ~C_FLAG;
            if (reg_a >= src_data) {
                reg_p |= C_FLAG;
            }
            last_n_value = last_z_value = reg_a - src_data;
        } break;
        case 0xDE: /* 0xDE - DEC ABSX */
        {
            uint16_t src_addr;
            uint8_t src_data;
            src_addr = cpu_abs_indexed_dummy_read(reg_x);
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data); // dummy
            src_data--;
            cpu_write(src_addr, src_data);
            last_n_value = last_z_value = src_data;
        } break;
        case 0xDF: /* 0xDF - DCP ABSX */
        {
            uint16_t src_addr;
            uint8_t src_data;

            cpu_add_cycles(CPU_ONE_CYCLE);  // pad to get 8 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE);  // pad to get 8 cycles total for the instruction
            src_addr = ABSX_GET_SRC_ADDR(); // cycles 2, 3
            src_data = cpu_read(src_addr);  // cycle 4
            src_data--;
            reg_p &= ~C_FLAG;
            if (reg_a >= src_data) {
                reg_p |= C_FLAG;
            }
            cpu_write(src_addr, src_data);
            last_n_value = last_z_value = reg_a - src_data;
        } break;
        case 0xE0: /* 0xE0 - CPX IMM  */
        {
            uint8_t src_data;
            uint8_t dest_data;

            src_data = IMM_GET_SRC_DATA();
            dest_data = reg_x - src_data;
            if (reg_x >= src_data) {
                reg_p |= C_FLAG;
            } else {
                reg_p &= ~C_FLAG;
            }
            last_n_value = last_z_value = dest_data;
        } break;
        case 0xE1: /* 0xE1 - SBC INDX */
        {
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            cpu_read(pc);
            src_data = INDX_GET_SRC_DATA();
            src_data ^= 0xFF;  // implement as ADC with inverted input
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xE3: /* 0xE3 - ISB INDX (Undocumented) */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            src_addr = INDX_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            src_data++;
            cpu_write(src_addr, src_data);
            src_data ^= 0xFF;  // implement as ADC with inverted input
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xE4: /* 0xE4 - CPX ZP */
        {
            uint8_t src_data;
            uint8_t dest_data;

            src_data = ZP_GET_SRC_DATA();
            dest_data = reg_x - src_data;
            if (reg_x >= src_data) {
                reg_p |= C_FLAG;
            } else {
                reg_p &= ~C_FLAG;
            }
            last_n_value = last_z_value = dest_data;
        } break;
        case 0xE5: /* 0xE5 - SBC ZP */
        {
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            src_data = ZP_GET_SRC_DATA();
            src_data ^= 0xFF;  // implement as ADC with inverted input
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xE6: /* 0xE6 - INC ZP */
        {
            uint8_t src_addr;
            uint8_t src_data;

            src_addr = ZP_GET_SRC_ADDR();
            src_data = cpu_read_zp(src_addr);
            cpu_write_zp(src_addr, src_data);
            src_data++;
            cpu_write_zp(src_addr, src_data);
            last_n_value = last_z_value = src_data;
        } break;
        case 0xE7: /* 0xE7 - ISB ZP (Undocumented)*/
        {
            uint8_t src_addr;
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 5 cycles total for the instruction
            src_addr = ZP_GET_SRC_ADDR();
            src_data = cpu_read_zp(src_addr);
            src_data++;
            cpu_write(src_addr, src_data);
            src_data ^= 0xFF;  // implement as ADC with inverted input
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xE8: /* 0xE8 - INX IMP */
            IMP_DUMMY_FETCH();
            reg_x++;
            last_n_value = last_z_value = reg_x;
            break;
        case 0xE9: /* 0xE9 - SBC IMM */
        case 0xEB: /* 0xEB - SBC IMM (Undocumented) */
        {
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            src_data = IMM_GET_SRC_DATA();
            src_data ^= 0xFF;  // implement as ADC with inverted input
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0x1A: /* 0x1A - NOP IMP (Undocumented) */
        case 0x3A: /* 0x3A - NOP IMP (Undocumented) */
        case 0x5A: /* 0x5A - NOP IMP (Undocumented) */
        case 0x7A: /* 0x7A - NOP IMP (Undocumented) */
        case 0xDA: /* 0xDA - NOP IMP (Undocumented) */
        case 0xFA: /* 0xFA - NOP IMP (Undocumented) */
        case 0xEA: /* 0xEA - NOP IMP */ cpu_read(pc); break;
        case 0xEC: /* 0xEC - CPX ABS */
        {
            uint8_t src_data;
            uint8_t dest_data;

            src_data = ABS_GET_SRC_DATA();
            dest_data = reg_x - src_data;
            if (reg_x >= src_data) {
                reg_p |= C_FLAG;
            } else {
                reg_p &= ~C_FLAG;
            }
            last_n_value = last_z_value = dest_data;
        } break;
        case 0xED: /* 0xED - SBC ABS */
        {
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            src_data = ABS_GET_SRC_DATA();
            src_data ^= 0xFF;  // implement as ADC with inverted input
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xEF: /* 0xEF - ISB ABS (Undocumented)*/
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 6 cycles total for the instruction
            src_addr = ABS_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            src_data++;
            cpu_write(src_addr, src_data);
            src_data ^= 0xFF;  // implement as ADC with inverted input
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xEE: /* 0xEE - INC ABS */
        {
            uint16_t src_addr;
            uint8_t src_data;
            src_addr = ABS_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data); // dummy write
            src_data++;
            cpu_write(src_addr, src_data);
            last_n_value = last_z_value = src_data;
        } break;
        case 0xF0: /* 0xF0 - BEQ REL */
        {
            int8_t branch_offset = (int8_t)cpu_read_pc_inc();
            // (the branch polls the NMI edge detector here: see nmi_poll_ready)

            if (last_z_value == 0) {
                uint16_t old_pc = pc;
                pc += branch_offset;
                cpu_branch_irq_delay(old_pc, pc);
                cpu_read(old_pc); // cycle 3: dummy read of the byte after the operand
                if ((pc ^ old_pc) & 0xFF00) {
                    // the high byte changed, indicates page crossing
                    // (the branch polls the NMI edge detector here: see nmi_poll_ready)
                    cpu_branch_irq_poll4();
                    cpu_read((old_pc & 0xFF00) | (pc & 0x00FF)); // cycle 4: dummy read before PCH is fixed
                }
            }
        } break;
        case 0xF1: /* 0xF1 - SBC INDY */
        {
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            src_data = indy_get_src_data_page_cross();
            src_data ^= 0xFF;  // implement as ADC with inverted input
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xF3: /* 0xF3 - ISB INDY (Undocumented) */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 8 cycles total for the instruction
            src_addr = INDY_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            src_data++;
            cpu_write(src_addr, src_data);
            src_data ^= 0xFF;  // implement as ADC with inverted input
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xF5: /* 0xF5 - SBC ZPX */
        {
            uint8_t src_addr;
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            src_addr = ZPX_GET_SRC_ADDR();
            cpu_read_zp(src_addr);
            src_data = cpu_read_zp(src_addr);
            src_data ^= 0xFF;  // implement as ADC with inverted input
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xF6: /* 0xF6 - INC ZPX */
        {
            uint8_t src_addr;
            uint8_t src_data;

            src_addr = ZPX_GET_SRC_ADDR();
            cpu_read_zp(src_addr);
            src_data = cpu_read_zp(src_addr);
            cpu_write_zp(src_addr, src_data); // dummy write
            src_data++;
            cpu_write(src_addr, src_data);
            last_n_value = last_z_value = src_data;
        } break;
        case 0xF7: /* 0xF7 - ISB ZPX (Undocumented) */
        {
            uint8_t src_addr;
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 6 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 6 cycles total for the instruction
            src_addr = ZPX_GET_SRC_ADDR();
            src_data = cpu_read_zp(src_addr);
            src_data++;
            cpu_write_zp(src_addr, src_data);
            src_data ^= 0xFF;  // implement as ADC with inverted input
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xF8: /* 0xF8 - SED IMP */
            IMP_DUMMY_FETCH();
            reg_p |= D_FLAG;
            break;
        case 0xF9: /* 0xF9 - SBC ABY */
        {
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            src_data = absy_get_src_data_page_cross();
            src_data ^= 0xFF;  // implement as ADC with inverted input
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xFB: /* 0xFB - ISB ABY */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            src_addr = ABSY_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            src_data++;
            cpu_write(src_addr, src_data);
            src_data ^= 0xFF;  // implement as ADC with inverted input
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xFD: /* 0xFD - SBC ABSX */
        {
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;
            src_data = absx_get_src_data_page_cross();
            src_data ^= 0xFF;  // implement as ADC with inverted input
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        case 0xFE: /* 0xFE - INC ABSX */
        {
            uint16_t src_addr;
            uint8_t src_data;
            src_addr = cpu_abs_indexed_dummy_read(reg_x);
            src_data = cpu_read(src_addr);
            cpu_write(src_addr, src_data); // dummy
            src_data++;
            cpu_write(src_addr, src_data);
            last_n_value = last_z_value = src_data;
        } break;
        case 0xFF: /* 0xFF - ISB ABSX (Undocumented) */
        {
            uint16_t src_addr;
            uint8_t src_data;
            uint8_t dest_data;
            uint8_t temp_flag;

            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            cpu_add_cycles(CPU_ONE_CYCLE); // pad to get 7 cycles total for the instruction
            src_addr = ABSX_GET_SRC_ADDR();
            src_data = cpu_read(src_addr);
            src_data++;
            cpu_write(src_addr, src_data);
            src_data ^= 0xFF;  // implement as ADC with inverted input
            dest_data = reg_a; /* save the previous value of Acc */
            reg_a += src_data + (reg_p & C_FLAG);
            reg_p &= ~(C_FLAG | V_FLAG); /* clear C and V flags */
            temp_flag = ((dest_data & src_data) | ((~reg_a) & src_data) | (dest_data & (~reg_a)));
            reg_p |= ((temp_flag & BIT_7) >> 7);

            temp_flag = ((dest_data & src_data & (~reg_a)) | ((~dest_data) & (~src_data) & reg_a));
            reg_p |= ((temp_flag & BIT_7) >> 1);
            last_n_value = last_z_value = reg_a;
        } break;
        default:
#ifdef ONE_DEBUGGER
            // Debugger build: a JAM/unsupported opcode stops in the debugger instead of aborting. PC stays on the opcode,
            // so resuming jams again (as the real CPU stays jammed) until PC is changed or the NES is reset.
            if (dbg_jam_hook) {
                pc--;
                dbg_jam_hook(pc, opcode);
                sched_break();
                break;
            }
#endif
            LOG_ERROR(CPU, "unsupported opcode %02X at %04X", opcode, (uint16_t)(pc - 1));
            assert(false);
        }

    //nmi_detect = nmi_edge_detect_output; // Poll the NMI edge detector
    nmi_detect = nmi_poll_ready; // Poll the NMI edge detector
    return nmi_detect;
}

bool cpu_clock_is_odd_cycle(void)
{
    return (cpu_clock_odd_tick == 1);
}

static void cpu_add_cycles(uint32_t cycles)
{
    assert(cycles == CPU_ONE_CYCLE);

    frame_clock += cycles;
    cpu_clock_odd_tick ^= 1;
    cpu_cycle++;

    if (glb_clock_stop && (cpu_cycle > glb_clock_stop) && !cpu_stop &&
        (0 == soft_reset_at)) { // -k counts from the (last) reset; 0 = run until stopped
        cpu_stop = true;
        LOG_INFO(EMU, "Stopping at CPU cycle %" PRIu64 " (-k)", cpu_cycle);
    }

    /* once a frame, check if user has hit any keys */
    if (frame_clock >= FRAME_TIME_CLKS) {
        frame_clock -= FRAME_TIME_CLKS;
        input_poll();
        if (script_active) {
            input_clear_overrides(); // a script sets this frame's buttons in its inputPolled callback
            script_event_fire(SCRIPT_EVENT_INPUT_POLLED);
        } // (not input_poll() itself: the debugger polls while stopped too)
    }

    // the cycle's 3 PPU dots, with the /NMI sample after the first one (cpu_sample_nmi()). One switch to the PPU
    // for all three: it takes the sample itself where it would hand the CPU its turn after the first dot
    // (cpu_ppu_turn()) and runs on. If the PPU is already past the first dot, the sample is taken here.
#ifdef FEATURE_PPU_CATCHUP
    catchup_init();
    if (catchup_on && !catchup_dma && (!cpu_trace_enabled || trace_sample > 0) && !script_active && apu_log == NULL &&
        !(savestate_pending && savestate_wants_ppu()) && ppu_clock_debt() + 12 <= catchup_limit) {
        // nothing the CPU does before the limit can see the PPU: owe its 12 clocks (3 dots), and take the /NMI sample
        // here (the line cannot change before the limit)
        ppu_add_cycles(12);
        catchup_cycles++;
        cpu_sample_nmi();
    } else
#endif
        if (ppu_add_cycles(4)) {
        nmi_sample_pending = true;
        co_switch(ppu_thread);
#ifdef FEATURE_PPU_CATCHUP
        catchup_limit = ppu_catchup_limit();
#endif
    } else {
        cpu_sample_nmi();
        if (ppu_add_cycles(8)) {
            co_switch(ppu_thread);
#ifdef FEATURE_PPU_CATCHUP
            catchup_limit = ppu_catchup_limit();
#endif
        }
    }


    // Latch the IRQ poll before this cycle's APU step: our APU runs at the end of a cycle (its step numbering is
    // aligned so $4015 reads match Mesen), so a flag it raises here belongs to the next cycle in Mesen's timing.
    irq_prev_run = irq_run;
    irq_run = (irq_requests != 0) && !(reg_p & I_FLAG);

    apu_step();
}

void cpu_cleanup()
{
    if (cpu_thread) {
        co_delete(cpu_thread);
        cpu_thread = NULL;
    }
}

// Press RESET at the next instruction boundary (debugger 'reset' command).
uint8_t *cpu_internal_ram(void)
{
    return internal_ram;
}

void cpu_request_soft_reset(void)
{
    soft_reset_at = (cpu_cycle > 0) ? cpu_cycle : 1;
}

void cpu_step()
{
    cpu_stepping = true;
}

uint8_t cpu_peek(uint16_t address)
{
#ifdef DEBUGGING_ENABLED
    return cpu_debug_read(address);
#else
    return (address < 0x2000) ? internal_ram[address & 0x7FF] : 0;
#endif
}

uint64_t cpu_get_cycle()
{
    return cpu_cycle;
}

#ifdef DEBUGGING_ENABLED
uint8_t cpu_debug_read_zp(uint16_t address);
uint16_t cpu_debug_get_pc()
{
    return pc;
}
uint8_t cpu_debug_get_reg_a()
{
    return reg_a;
}
uint8_t cpu_debug_get_reg_x()
{
    return reg_x;
}
uint8_t cpu_debug_get_reg_y()
{
    return reg_y;
}
uint8_t cpu_debug_get_sp()
{
    return sp;
}
uint64_t cpu_debug_get_cycle()
{
    return cpu_cycle;
}

void cpu_debug_set_pc(uint32_t new_pc)
{
    pc = new_pc;
}
void cpu_debug_set_reg_a(uint8_t new_a)
{
    reg_a = new_a;
}
void cpu_debug_set_reg_x(uint8_t new_x)
{
    reg_x = new_x;
}
void cpu_debug_set_reg_y(uint8_t new_y)
{
    reg_y = new_y;
}
void cpu_debug_set_reg_sp(uint8_t new_sp)
{
    sp = new_sp;
}

uint8_t cpu_debug_get_reg_p()
{
    reg_p &= (~(N_FLAG | Z_FLAG));   // Clear the NZ flags
    reg_p |= (last_n_value & BIT_7); // If last value was negative, set N_FLAG
    if (last_z_value == 0) {
        reg_p |= Z_FLAG;
    }
    return reg_p;
}

void cpu_debug_set_reg_p(uint8_t new_p)
{
    reg_p = new_p;

    if (Z_FLAG == (reg_p & Z_FLAG)) {
        last_n_value = last_z_value = 0;
    } else {
        last_n_value = last_z_value = reg_p;
    }
}


uint8_t cpu_debug_read(uint16_t address)
{
#ifdef TRACE_TYPE_MESEN
    uint8_t value = 0x00;
#else
    uint8_t value = 0xFF;
#endif

    if (address < 0x2000) {
        value = cpu_debug_read_zp(address & 0x7FF);
    } else if (address < 0x4000) {
        value = ppu_debug_read_regs(address);
    } else if (address <= 0x4017) {
        value = rp2a03_debug_read(address);
    } else if (address < 0x6000) {
        value = (uint8_t)(address >> 8); // unmapped: Mesen's debugger shows open bus as the address high byte
        if (mapper_has_exp && address >= 0x4020) {
            (void)mapper_debug_read_exp(address, &value);
        }
    } else {
        // mapper
        value = mapper_debug_read_prg(address);
    }
    return value;
}

uint8_t cpu_debug_read_zp(uint16_t address)
{
    uint8_t value;
    value = internal_ram[address & 0x7FF];
    return value;
}

uint16_t cpu_debug_read_word(uint16_t address)
{
    uint16_t value;
    uint16_t upper_byte;

    value = (uint16_t)cpu_debug_read(address);
    if ((address & 0x00FF) == 0x00FF) {
        address = 0;
    } else {
        address++;
    }
    upper_byte = ((uint16_t)cpu_debug_read(address)) << 8;
    value |= upper_byte;
    return value;
}

uint16_t cpu_debug_read_word_zp(uint8_t address)
{
    uint16_t value;
    uint16_t upper_byte;

    value = (uint16_t)cpu_debug_read_zp(address);
    address = (address + 1) & 0xFF;
    upper_byte = ((uint16_t)cpu_debug_read_zp(address)) << 8;
    value |= upper_byte;

    return value;
}

// Debugger memory edit: internal RAM and the cartridge's PRG-RAM ($6000-$7FFF, written directly, whatever the
// mapper's RAM enable bits say). Anything else would reach PPU/APU/mapper registers or ROM, so it is refused.
bool cpu_debug_write(uint16_t address, uint8_t value)
{
    if (address < 0x2000) {
        internal_ram[address & 0x7FF] = value;
        return true;
    }
    if (address >= 0x6000 && mapper_debug_write_prg(address, value)) {
        return true; // (banked cartridge RAM, as mapped now)
    }
    if (address >= 0x6000 && address < 0x8000) {
        size_t size;
        uint8_t *prg_ram = mapper_debug_prg_ram(&size);
        if (prg_ram != NULL && (size_t)(address - 0x6000) < size) {
            prg_ram[address - 0x6000] = value;
            return true;
        }
    }
    return false;
}


#define NUM_BREAKPOINTS (16)


#endif //DEBUGGING_ENABLED

// --- save states -------------------------------------------------------------------------------------------------

static bool resume_hook_first;

static void cpu_resume_entry(void)
{
    cpu_main_loop(resume_hook_first);
}

void cpu_state_restart(bool boundary_hook_first)
{
    if (cpu_thread != NULL) {
        co_delete(cpu_thread);
    }
    resume_hook_first = boundary_hook_first;
    cpu_thread = co_create(65536 * sizeof(void *), cpu_resume_entry);
    cpu_stop = false;
}

void cpu_state(state_io *io)
{
    STATE(io, internal_ram);
    STATE(io, pc);
    STATE(io, reg_a);
    STATE(io, reg_x);
    STATE(io, reg_y);
    STATE(io, sp);
    STATE(io, reg_p);
    STATE(io, last_n_value);
    STATE(io, last_z_value);
    STATE(io, previous_nmi_requested);
    STATE(io, nmi_sample_pending);
    STATE(io, nmi_requested);
    STATE(io, nmi_edge_detect_internal);
    STATE(io, nmi_edge_detect_output);
    STATE(io, nmi_poll_ready);
    STATE(io, irq_requests);
    STATE(io, irq_run);
    STATE(io, irq_prev_run);
    STATE(io, cpu_open_bus);
    STATE(io, cpu_internal_bus);
    STATE(io, frame_clock);
    STATE(io, cpu_cycle);
    STATE(io, cpu_clock_odd_tick);
    // not the machine: the emulator's own controls
    STATE_SKIP(cpu_thread);        // the coroutine (restarted by cpu_state_restart())
    STATE_SKIP(cpu_stop);          // the emulation is ending
    STATE_SKIP(cpu_stepping);      // (unused)
    STATE_SKIP(cpu_trace_enabled); // -t
    STATE_SKIP(trace_sample);      // --trace-sample
    STATE_SKIP(trace_countdown);
    STATE_SKIP(trace_rng);
    STATE_SKIP(glb_clock_stop); // -k
    STATE_SKIP(start_state);    // -p / -R
    STATE_SKIP(soft_reset_at);  // -r
#ifdef FEATURE_PPU_CATCHUP
    STATE_SKIP(catchup_on);     // (a state is taken after a catch-up: the PPU is where the CPU is)
    STATE_SKIP(catchup_limit);  // ...
    STATE_SKIP(catchup_dma);    // ...
    STATE_SKIP(catchup_cycles); // statistics
    STATE_SKIP(catchup_flushes);
    STATE_SKIP(catchup_stats);
#endif
    STATE_SKIP(soft_reset_queue);
    STATE_SKIP(soft_reset_count);
    STATE_SKIP(soft_reset_next);
    STATE_SKIP(resume_hook_first);
}
