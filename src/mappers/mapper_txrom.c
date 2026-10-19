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

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "mapper.h"
#include "ines.h"
#include "bits.h"
#include "cpu.h"
#include "savestate.h"
#include "settings.h"

static uint8_t *prg_ram_storage = NULL; /* holds the physical data */
static uint32_t prg_ram_size_bytes = 0; /* 0: no PRG-RAM (NES 2.0 can say so); else a power of two up to 8KB */
static uint8_t *prg_rom_storage = NULL; /* holds the physical data */
static uint32_t prg_rom_size_bytes = 0;
static uint32_t prg_slot_offset[4]; // offset 0=$8000, 1=$A000, 2=$C000, 3=$E000
static bool chr_is_ram = false;
static uint8_t *chr_storage = NULL; /* CHR-ROM, or 8KB of CHR-RAM; TQROM: the CHR-ROM and then 8KB of CHR-RAM */
static uint32_t chr_size_bytes = 0; /* all of chr_storage */
static uint32_t chr_rom_bytes = 0;  /* the part of it the 1KB banks wrap around: all of it, but TQROM's RAM */
static uint32_t chr_ram_start =
    0; /* from this offset on chr_storage is RAM: 0 (CHR-RAM), chr_size_bytes (none) or TQROM's */

// The boards built on the MMC3 chip. TxSROM and TQROM use it with a different use of the CHR bank bits; the MMC6 (StarTropics,
// StarTropics II) replaces the 8KB PRG-RAM with 1KB and its own enable bits.
typedef enum {
    BOARD_MMC3,
    BOARD_TXSROM,
    BOARD_TQROM,
    BOARD_MMC6,
    BOARD_NAMCO108,
    BOARD_NAMCO88, // mapper 88: a Namco 108 whose CHR A16 is set by R2-R5 and clear for R0 / R1
    BOARD_MULTI37,
    BOARD_MULTI47
} txrom_board_t;
static txrom_board_t board = BOARD_MMC3;
static bool board_is_namco108(void)
{
    return BOARD_NAMCO108 == board || BOARD_NAMCO88 == board;
}
#define MMC6_RAM_SIZE 1024

// Multicarts (mapper 37: Super Mario Bros. + Tetris + Nintendo World Cup, mapper 47: Super Spike V'Ball + Nintendo World Cup): an MMC3 whose
// banks are confined to a block of the ROMs picked by the outer register ($6000-$7FFF, when the PRG-RAM is writable): bank numbers are
// masked and offset (8KB PRG banks, 1KB CHR banks), the fixed PRG banks are the last two of the block.
static uint8_t outer_reg = 0;
static uint16_t prg_mask = 0xFF, prg_offset = 0, chr_mask = 0xFF, chr_offset = 0;
static void multicart_update_outer(void)
{
    if (BOARD_MULTI47 == board) {
        prg_mask = 0x0F;
        prg_offset = (uint16_t)((outer_reg & 1u) << 4);
        chr_mask = 0x7F;
        chr_offset = (uint16_t)((outer_reg & 1u) << 7);
    } else if (BOARD_MULTI37 == board) {
        // From Mesen's trace of the multicart (every instruction byte after each outer write matches the ROM with these): outer 0-2 the first
        // 64KB (the menu; the power-on state), 3 the next 64KB (Super Mario Bros.), 4-7 the second 128KB (Nintendo World Cup); CHR: bit 2 picks
        // the half (the CHR side is not in a CPU trace: unchecked). Outer 1, 2 and 7 were never written in the trace.
        prg_mask = (outer_reg & 4u) ? 0x0F : 0x07;
        prg_offset = (outer_reg & 4u) ? 0x10 : (3 == (outer_reg & 7u)) ? 0x08 : 0x00;
        chr_mask = 0x7F;
        chr_offset = (outer_reg & 4u) ? 0x80 : 0x00;
    }
}
static uint32_t chr_slot_offset[8]; // 1KB slots: offset 0=PPU $0000, 1=$0400, ... 7=$1C00
static bool four_screen = false;    // header flag 6 bit 3: $A000 is ignored (the cartridge has its own VRAM)
static uint16_t ciram_page[4];      // CIRAM offset of each name table ($2000, $2400, $2800, $2C00)
static uint8_t four_screen_ram
    [0x1000]; // four-screen boards: 4KB of name table RAM on the cartridge (instead of the console's CIRAM)

// registers
static uint8_t R[8] = {0};
union mmc3_regs_u {
    uint8_t reg[8]; // Access via array indices
    struct {        // Access via individual names
        uint8_t bank_select;
        uint8_t bank_data;
        uint8_t nametable_arrangement;
        uint8_t prg_ram_protect;
        uint8_t irq_latch;
        uint8_t irq_reload;
        uint8_t irq_disable;
        uint8_t irq_enable;
    }; // Anonymous struct groups the variables
};
static union mmc3_regs_u mmc_regs;

// Scanline IRQ: an 8-bit counter clocked by rising edges of PPU A12 (with the usual pattern tables, once per
// scanline at the first sprite fetch). The MMC3 ignores a rise unless A12 was low for a while (it counts M2 edges),
// so the eight A12 toggles of the background fetches do not clock it. The threshold is Mesen's (more than 10 dots).
#define A12_MIN_LOW_DOTS 10
static uint8_t irq_counter = 0;
static bool irq_reload = false;   // $C001 was written: reload the counter from the latch on the next clock
static bool irq_enabled = false;  // $E001 enables, $E000 disables (and acknowledges)
static uint64_t a12_fall_dot = 0; // when A12 last went low (PPU dots)

// Byte offsets of the four 8KB slots, recomputed when bank_select, R6 or R7 changes.
//   bank_select bit 6 = 0:  $8000 = R6, $A000 = R7, $C000 = second-last, $E000 = last
//   bank_select bit 6 = 1:  $8000 = second-last, $A000 = R7, $C000 = R6, $E000 = last
// '% banks' wraps bank numbers larger than the ROM.
static void mapper_txrom_update_prg_banks(void)
{
    uint32_t banks = prg_rom_size_bytes >> 13;              // number of 8KB banks
    uint32_t r6_slot = (mmc_regs.bank_select & BIT_6) >> 5; // 0 ($8000) or 2 ($C000)

    if (BOARD_MULTI37 == board || BOARD_MULTI47 == board) {
        prg_slot_offset[r6_slot] = ((((uint32_t)R[6] & prg_mask) | prg_offset) % banks) << 13;
        prg_slot_offset[1] = ((((uint32_t)R[7] & prg_mask) | prg_offset) % banks) << 13;
        prg_slot_offset[r6_slot ^ 2] = (((0xFEu & prg_mask) | prg_offset) % banks) << 13;
        prg_slot_offset[3] = (((0xFFu & prg_mask) | prg_offset) % banks) << 13;
        return;
    }
    prg_slot_offset[r6_slot] = ((R[6] & 0x3F) % banks) << 13;
    prg_slot_offset[1] = ((R[7] & 0x3F) % banks) << 13;
    prg_slot_offset[r6_slot ^ 2] = (banks - 2) << 13;
    prg_slot_offset[3] = (banks - 1) << 13;
}

// Byte offsets of the eight 1KB CHR slots, recomputed when bank_select or R0-R5 changes.
//   bank_select bit 7 = 0:  $0000-$07FF = R0 (2KB), $0800-$0FFF = R1 (2KB), $1000-$1FFF = R2-R5 (1KB each)
//   bank_select bit 7 = 1:  the two halves swap ($0000-$0FFF = R2-R5, $1000-$1FFF = R0, R1)
// The 2KB banks ignore bit 0 of R0/R1.  '% banks' wraps bank numbers larger than the CHR size.
static void mapper_txrom_update_chr_banks(void)
{
    uint32_t banks = chr_rom_bytes >> 10;                       // number of 1KB banks
    uint32_t invert = (mmc_regs.bank_select & BIT_7) ? 4u : 0u; // slot index XOR 4 swaps the halves

    for (uint32_t slot = 0; slot < 8; slot++) {
        uint32_t bank = (slot < 4) ? ((R[slot >> 1] & 0xFEu) | (slot & 1)) : R[slot - 2];
        if (BOARD_TXSROM == board) {
            bank &= 0x7F; // (bit 7 is the name table)
        } else if (BOARD_TQROM == board && (bank & BIT_6)) {
            chr_slot_offset[slot ^ invert] = chr_rom_bytes + ((bank & 7u) << 10); // bit 6: 1KB of the CHR-RAM
            continue;
        } else if (BOARD_TQROM == board) {
            bank &= 0x3F;
        } else if (BOARD_MULTI37 == board || BOARD_MULTI47 == board) {
            bank = (bank & chr_mask) | chr_offset;
        } else if (BOARD_NAMCO88 == board) {
            bank = (slot < 4) ? (bank & 0x3Fu)
                              : ((bank & 0x3Fu) | 0x40u); // the 2KB banks in the first 64KB, the 1KB in the second
        }
        chr_slot_offset[slot ^ invert] = (bank % banks) << 10;
    }
}

// Mirroring: CIRAM A0-A9 come from PPU A0-A9, and $A000 bit 0 picks CIRAM A10:
//   0: vertical   -> A10 = PPU A10  (pages 0, 1, 0, 1)
//   1: horizontal -> A10 = PPU A11  (pages 0, 0, 1, 1)
// Recomputed on $A000 writes, so a name-table access is a single lookup.  Four-screen boards have 2KB more VRAM on
// the cartridge; ONE has only the 2KB of CIRAM, so they get vertical.
static void mapper_txrom_update_mirroring(void)
{
    if (BOARD_TXSROM == board) {
        // TxSROM: bit 7 of the CHR bank register that drives the same 1KB of the pattern tables (R0 / R1 or R2-R5 when
        // inverted) picks the CIRAM page of each name table; $A000 does nothing
        unsigned invert = (mmc_regs.bank_select & BIT_7) ? 4u : 0u;
        for (unsigned nt = 0; nt < 4; nt++) {
            unsigned slot = nt ^ invert;
            uint8_t bank = (slot < 4) ? R[slot >> 1] : R[slot - 2];
            ciram_page[nt] = (uint16_t)(((bank >> 7) & 1) << 10);
        }
        return;
    }
    if (four_screen) {
        for (unsigned nt = 0; nt < 4; nt++) {
            ciram_page[nt] = 0x800; // not CIRAM: mapper_txrom_read_nt() / write_nt()
        }
        return;
    }
    unsigned horizontal = mmc_regs.nametable_arrangement & 1;

    for (unsigned nt = 0; nt < 4; nt++) {
        ciram_page[nt] = (uint16_t)(((nt >> horizontal) & 1) << 10);
    }
}

static void mapper_txrom_deinit(void)
{
    LOG_DEBUG(MAPPER, "free'ing PRG and CHR.");
    free(prg_rom_storage);
    prg_rom_storage = NULL;
    prg_rom_size_bytes = 0;
    free(prg_ram_storage);
    prg_ram_storage = NULL;
    prg_ram_size_bytes = 0;
    free(chr_storage);
    chr_storage = NULL;
    chr_size_bytes = 0;
    chr_rom_bytes = 0;
    chr_ram_start = 0;
    board = BOARD_MMC3;
    memset(prg_slot_offset, 0, sizeof(prg_slot_offset));
    memset(chr_slot_offset, 0, sizeof(chr_slot_offset));
    memset(R, 0, sizeof(R));
    memset(&mmc_regs, 0, sizeof(mmc_regs));
    irq_counter = 0;
    irq_reload = false;
    irq_enabled = false;
    a12_fall_dot = 0;
    cpu_irq_request(false, BIT_2);
}

// MMC6: the 1KB RAM sits at $7000-$7FFF (mirrored every 1KB), and is enabled by $8000 bit 5. $A001 enables each 512-byte half
// separately: bit 5 / 4 read / write $7000-$71FF, bit 7 / 6 read / write $7200-$73FF (and their mirrors).
static bool mmc6_ram_access(uint16_t address, bool write)
{
    if (address < 0x7000 || address >= 0x8000 || !(mmc_regs.bank_select & BIT_5)) {
        return false;
    }
    bool upper = (address & 0x200) != 0;
    uint8_t enable = write ? (upper ? BIT_6 : BIT_4) : (upper ? BIT_7 : BIT_5);
    return (mmc_regs.prg_ram_protect & enable) != 0;
}

static uint8_t mapper_txrom_read_prg(uint16_t address)
{
    uint8_t value;
    if (address >= 0x8000) { // first: nearly every read is an opcode or operand fetch from ROM
        value = prg_rom_storage[prg_slot_offset[(address >> 13) & 3] | (address & 0x1FFF)];
    } else if (BOARD_MMC6 == board) {
        value = mmc6_ram_access(address, false) ? prg_ram_storage[address & (MMC6_RAM_SIZE - 1)] : cpu_get_open_bus();
    } else if ((address >= 0x6000) && (mmc_regs.prg_ram_protect & BIT_7) && (0 != prg_ram_size_bytes)) {
        value = prg_ram_storage[address & (prg_ram_size_bytes - 1)]; // (less than 8KB: mirrored)
    } else {
        value = cpu_get_open_bus(); // $4019-$5FFF, PRG-RAM disabled, or none
    }
    return value;
}

static void mapper_txrom_write_prg(uint16_t address, uint8_t value)
{
    if (address < 0x6000) {
        // do nothing
    } else if (address < 0x8000) {
        if ((BOARD_MULTI37 == board || BOARD_MULTI47 == board) &&
            ((mmc_regs.prg_ram_protect & (BIT_7 | BIT_6)) == BIT_7)) {
            outer_reg = value & ((BOARD_MULTI47 == board) ? 1u : 7u);
            multicart_update_outer();
            mapper_txrom_update_prg_banks();
            mapper_txrom_update_chr_banks();
        } else if (BOARD_MMC6 == board) {
            if (mmc6_ram_access(address, true)) {
                prg_ram_storage[address & (MMC6_RAM_SIZE - 1)] = value;
            }
        } else if (((mmc_regs.prg_ram_protect & (BIT_7 | BIT_6)) == BIT_7) && (0 != prg_ram_size_bytes)) {
            prg_ram_storage[address & (prg_ram_size_bytes - 1)] = value;
        }
    } else if (board_is_namco108() && (address >= 0xA000)) {
        // Namco 108: nothing but the bank registers (mirroring from the header, no IRQ, no RAM protection)
    } else {
        int reg_index = ((address & 0x6000) >> 12) | (address & BIT_0);
        if (board_is_namco108()) {
            value = (0 == reg_index) ? (value & 0x07) : value; // (no PRG / CHR inversion bits)
        }
        mmc_regs.reg[reg_index] = value;
        switch (reg_index) {
        case 0: // bank_select
            mapper_txrom_update_prg_banks();
            mapper_txrom_update_chr_banks();
            mapper_txrom_update_mirroring(); // (TxSROM: depends on the CHR inversion)
            break;
        case 1: { // bank_data
            uint8_t reg_num = mmc_regs.bank_select & 0x07;
            if (board_is_namco108()) {
                value &= (reg_num >= 6) ? 0x0F : 0x3F; // 4-bit PRG banks, 6-bit CHR banks
            }
            R[reg_num] = value;
            if (reg_num >= 6) {
                mapper_txrom_update_prg_banks();
            } else {
                mapper_txrom_update_chr_banks();
                mapper_txrom_update_mirroring();
            }
            break;
        }
        case 2: // nametable_arrangement (not on TxSROM)
            mapper_txrom_update_mirroring();
            break;
        case 3: // prg_ram_protect
            break;
        case 4: // irq_latch (the value is used at the next reload)
            break;
        case 5: // irq_reload: clear the counter; the next clock reloads it from the latch
            irq_counter = 0;
            irq_reload = true;
            break;
        case 6: // irq_disable, and acknowledge a pending IRQ
            irq_enabled = false;
            cpu_irq_request(false, BIT_2);
            break;
        case 7: // irq_enable
            irq_enabled = true;
            break;
        default: assert(0);
        };
    }
}

// mapper.mmc3_irq: which MMC3 revision's IRQ to emulate (see mapper_txrom_ppu_a12)
static bool irq_revision_a = false;
static bool rom_irq_revision_a = false; // NES 2.0 submapper 4 (MMC3A): revision A whatever the setting says

static bool apply_mmc3_irq(const setting *s, const char *value, char *error, size_t error_size)
{
    (void)s;
    if (error_size > 0) { // (never fails: settings.c only passes one of the choices)
        error[0] = '\0';
    }
    irq_revision_a = (0 == strcmp(value, "A"));
    return true;
}

static const char *const mmc3_irq_choices[] = {"B", "A", NULL};
static const setting txrom_table[] = {
    {"mapper.mmc3_irq", SETTING_CHOICE, "B", "MMC3 IRQ revision", NULL,
     "B: the IRQ fires whenever a clock leaves the counter at 0 (MMC3B/C, most cartridges; Mesen's default)\n"
     "A: only when the counter goes from nonzero to 0, or after a $C001 reload (older MMC3A boards)",
     0, 0, mmc3_irq_choices, 0, apply_mmc3_irq, 0},
};

const setting_group *mapper_txrom_settings(void)
{
    static const setting_group group = {"cartridge", "Cartridge", "Mapper hardware variants.", txrom_table,
                                        (int)(sizeof(txrom_table) / sizeof(txrom_table[0]))};
    return &group;
}

// A12 rising edge after a long enough low: clock the counter (reload when 0 or after $C001, else decrement), and
// request the IRQ when it is 0 and enabled. Revision B (the default, Mesen's): a reload to 0 also fires, so a latch
// of 0 fires on every scanline. Revision A: only a decrement to 0, or a reload to 0 after $C001, fires
// (mmc3_test_2 6-MMC3_alt).
static void mapper_txrom_ppu_a12(bool high, uint64_t dot)
{
    if (!high) {
        a12_fall_dot = dot;
        return;
    }
    if ((int64_t)(dot - a12_fall_dot) <= A12_MIN_LOW_DOTS) { // (negative after a reset: also not counted)
        return;
    }
    bool was_nonzero = (0 != irq_counter);
    bool reloaded_by_c001 = irq_reload;
    if ((0 == irq_counter) || irq_reload) {
        irq_counter = mmc_regs.irq_latch;
    } else {
        irq_counter--;
    }
    irq_reload = false;
    bool revision_a = irq_revision_a || rom_irq_revision_a;
    bool fire = (0 == irq_counter) && (!revision_a || was_nonzero || reloaded_by_c001);
    if (fire && irq_enabled) {
        cpu_irq_request(true, BIT_2);
    }
}

// PPU catch-up: after how many counted A12 rises can an IRQ be requested? (A decrement to 0, or a reload to 0 with a
// latch of 0, fires; revision A fires in fewer cases, never earlier.) UINT32_MAX: not while the IRQ is off, nor while
// it is already requested (only a CPU write, $E000, takes it back).
static uint32_t mapper_txrom_a12_rises_to_irq(void)
{
    if (!irq_enabled || cpu_irq_requested(BIT_2)) {
        return UINT32_MAX;
    }
    if ((0 == irq_counter) || irq_reload) {
        return (0 == mmc_regs.irq_latch) ? 1u : 1u + mmc_regs.irq_latch;
    }
    return irq_counter;
}

// Debugger/trace peek: like a read, but disabled (or missing) PRG-RAM shows as the address high byte (Mesen's debugger, and
// cpu_debug_read() for $4018-$5FFF)
static uint8_t mapper_txrom_debug_read_prg(uint16_t address)
{
    if (BOARD_MMC6 == board && address < 0x8000) {
        // (Mesen's debugger shows the RAM at $7000-$7FFF whatever the enable bits say)
        return (address >= 0x7000) ? prg_ram_storage[address & (MMC6_RAM_SIZE - 1)] : (uint8_t)(address >> 8);
    }
    if ((address < 0x8000) && (!(mmc_regs.prg_ram_protect & BIT_7) || (0 == prg_ram_size_bytes))) {
        return (uint8_t)(address >> 8);
    }
    return mapper_txrom_read_prg(address);
}

static uint8_t mapper_txrom_read_chr(uint16_t address)
{
    assert(address < 0x2000);
    return chr_storage[chr_slot_offset[(address >> 10) & 7] | (address & 0x3FF)];
}

static uint8_t mapper_txrom_read_nt(uint16_t address)
{
    return four_screen_ram[address & 0xFFF];
}

static void mapper_txrom_write_nt(uint16_t address, uint8_t value)
{
    four_screen_ram[address & 0xFFF] = value;
}

static void mapper_txrom_write_chr(uint16_t address, uint8_t value)
{
    assert(address < 0x2000);
    uint32_t offset = chr_slot_offset[(address >> 10) & 7] | (address & 0x3FF);
    if (offset >= chr_ram_start) {
        chr_storage[offset] = value;
    } else {
        LOG_TRACE(MAPPER, "Attempt to write to chr_rom  [%04X] = %02X", address, value);
    }
}

// input is the PPU address of a name-table access, the result its CIRAM address (see mapper_txrom_update_mirroring)
static uint16_t mapper_txrom_get_ciram_addr(uint16_t addr)
{
    return ciram_page[(addr >> 10) & 3] | (addr & 0x3FF);
}

static uint8_t mapper_txrom_debug_read_chr(uint16_t address)
{
    return mapper_txrom_read_chr(address);
}

static int32_t mapper_txrom_debug_chr_offset(uint16_t address)
{
    return (int32_t)(chr_slot_offset[(address >> 10) & 7] | (address & 0x3FF));
}

static const uint8_t *mapper_txrom_debug_chr_data(size_t *size)
{
    *size = chr_size_bytes;
    return chr_storage;
}

static int32_t mapper_txrom_debug_prg_offset(uint16_t address)
{
    return (int32_t)(prg_slot_offset[(address >> 13) & 3] | (address & 0x1FFF));
}

static uint8_t *mapper_txrom_debug_prg_ram(size_t *size)
{
    *size = prg_ram_size_bytes;
    return prg_ram_storage;
}

// battery-backed PRG-RAM for the .sav file (mapper.c writes it only when the header has the battery flag)
static void mapper_txrom_get_sav_data(sav_data_t *sav)
{
    sav->data = prg_ram_storage; // still owned by this mapper
    sav->size = prg_ram_size_bytes;
}

// Save states: the registers, the bank offsets they select, and the RAM
static void mapper_txrom_state(state_io *io)
{
    STATE(io, R);
    STATE(io, mmc_regs);
    STATE(io, prg_slot_offset);
    STATE(io, chr_slot_offset);
    STATE(io, ciram_page);
    STATE(io, outer_reg);
    STATE(io, prg_mask);
    STATE(io, prg_offset);
    STATE(io, chr_mask);
    STATE(io, chr_offset);
    STATE(io, four_screen_ram);
    if (prg_ram_storage != NULL) {
        state_bytes(io, "txrom.prg_ram", prg_ram_storage, prg_ram_size_bytes);
    }
    if (chr_ram_start < chr_size_bytes) {
        state_bytes(io, "txrom.chr_ram", chr_storage + chr_ram_start, chr_size_bytes - chr_ram_start);
    }
    STATE_SKIP(prg_ram_storage); // (the pointers and sizes come from the ROM)
    STATE_SKIP(prg_ram_size_bytes);
    STATE_SKIP(prg_rom_storage);
    STATE_SKIP(prg_rom_size_bytes);
    STATE_SKIP(chr_storage);
    STATE_SKIP(chr_size_bytes);
    STATE_SKIP(chr_rom_bytes);
    STATE_SKIP(chr_ram_start);
    STATE_SKIP(board);
    STATE_SKIP(chr_is_ram);
    STATE_SKIP(four_screen);
    STATE_SKIP(irq_revision_a);     // (a setting)
    STATE_SKIP(rom_irq_revision_a); // (from the ROM's header)
    STATE(io, irq_counter);
    STATE(io, irq_reload);
    STATE(io, irq_enabled);
    STATE(io, a12_fall_dot);
}

// The NES 2.0 header fields the MMC3 uses (mapper_nes2_info()): the submapper, and one RAM chip at $6000,
// battery-backed or not. Sets prg_ram_size_bytes, rom_irq_revision_a and *chr_ram_size; false (logged) if the ROM
// needs something ONE cannot do.
static bool mapper_txrom_nes2_header(const ines_header *h, uint32_t *chr_ram_size)
{
    nes2_info_t nes2;
    if (!mapper_nes2_info(h, &nes2)) {
        return false;
    }
    switch (nes2.submapper) {
    case 0: break; // MMC3 (revision B/C)
    case 2: break; // (deprecated: MMC3C) the same
    case 4: rom_irq_revision_a = true; break;
    case 1: board = BOARD_MMC6; break;
    case 3: LOG_WARN(MAPPER, "NES 2.0 submapper 3 (MC-ACC) is not supported: run as an MMC3"); break;
    default: LOG_WARN(MAPPER, "NES 2.0 submapper %u is not supported: run as an MMC3", nes2.submapper); break;
    }

    if ((0 != nes2.prg_ram_size) && (0 != nes2.prg_nvram_size)) {
        LOG_WARN(MAPPER, "NES 2.0 header asks for both PRG-RAM and battery RAM: using the battery RAM");
    }
    prg_ram_size_bytes = (0 != nes2.prg_nvram_size) ? nes2.prg_nvram_size : nes2.prg_ram_size;
    if (prg_ram_size_bytes > 8 * 1024) {
        LOG_WARN(MAPPER, "NES 2.0 PRG-RAM of %u bytes: the MMC3 maps 8KB", prg_ram_size_bytes);
        prg_ram_size_bytes = 8 * 1024;
    }
    *chr_ram_size = nes2.chr_ram_size;
    return true;
}

// The MMC6 games: the iNES header does not tell them from MMC3 games (mapper 4, no submapper), so they are known by the
// CRC-32 of their PRG + CHR (No-Intro)
static bool is_mmc6_game(void)
{
    static const uint32_t mmc6_crcs[] = {
        0x889129CB, // StarTropics (USA)
        0xD054FFB0, // Zoda's Revenge - StarTropics II (USA)
    };
    uint32_t crc = mapper_rom_crc();
    for (size_t i = 0; i < sizeof(mmc6_crcs) / sizeof(mmc6_crcs[0]); i++) {
        if (crc == mmc6_crcs[i]) {
            return true;
        }
    }
    return false;
}

bool mapper_txrom_init_ines(ines_data *ines_file_data, mapper_info_t *p_mapper_info, sav_data_t *sav_data,
                            unsigned mapper_number)
{
    assert(ines_file_data != NULL);
    assert(p_mapper_info != NULL);
    assert(sav_data != NULL);

    board = (TxSROM == mapper_number) ? BOARD_TXSROM : (TQROM == mapper_number) ? BOARD_TQROM : BOARD_MMC3;
    if (Namco108 == mapper_number) {
        board = BOARD_NAMCO108;
    } else if (88 == mapper_number) {
        board = BOARD_NAMCO88;
    } else if (37 == mapper_number) {
        board = BOARD_MULTI37;
    } else if (47 == mapper_number) {
        board = BOARD_MULTI47;
    }
    outer_reg = 0;
    prg_mask = chr_mask = 0xFF;
    prg_offset = chr_offset = 0;
    multicart_update_outer();
    if (BOARD_MMC3 == board && is_mmc6_game()) {
        board = BOARD_MMC6;
    }

    uint32_t chr_ram_size = 8 * 1024; // with no CHR-ROM (iNES 1.0: always 8KB)
    rom_irq_revision_a = false;
    if (mapper_is_nes2(&ines_file_data->header)) {
        if (!mapper_txrom_nes2_header(&ines_file_data->header, &chr_ram_size)) {
            return false;
        }
    } else {
        prg_ram_size_bytes = 8 * 1024; // assume 8KB for iNES 1.0
    }
    if (board_is_namco108() || BOARD_MULTI37 == board || BOARD_MULTI47 == board) {
        prg_ram_size_bytes = 0; // no RAM (the multicarts use $6000-$7FFF for their outer register)
    }
    if (BOARD_MMC6 == board) {
        LOG_DEBUG(MAPPER, "MMC6: 1KB PRG-RAM");
        prg_ram_size_bytes = MMC6_RAM_SIZE;
    }
    if (0 != prg_ram_size_bytes) {
        prg_ram_storage = calloc(1, prg_ram_size_bytes);
        if (NULL == prg_ram_storage) {
            LOG_ERROR(MAPPER, "unable to allocate PRG-RAM storage");
            return false;
        }
        if ((ines_file_data->header.flags_6 & BIT_1) && (sav_data->size > 0)) { // battery: restore the .sav file
            if (sav_data->size != prg_ram_size_bytes) {
                LOG_WARN(MAPPER, ".sav file is %zu bytes, expected %u; using what fits", sav_data->size,
                         prg_ram_size_bytes);
            }
            memcpy(prg_ram_storage, sav_data->data,
                   (sav_data->size < prg_ram_size_bytes) ? sav_data->size : prg_ram_size_bytes);
        }
    }

    prg_rom_size_bytes = 16 * 1024 * ines_file_data->header.prg_16kb_banks;
    LOG_DEBUG(MAPPER, "PRG size: %d (0x%X)", prg_rom_size_bytes, prg_rom_size_bytes);
    if ((0 == ines_file_data->header.prg_16kb_banks) || (ines_file_data->header.prg_16kb_banks > 32)) {
        LOG_ERROR(MAPPER, "unsupported PRG size");
        free(prg_ram_storage);
        prg_ram_storage = NULL;
        return false;
    }
    prg_rom_storage = calloc(1, prg_rom_size_bytes);
    if (NULL == prg_rom_storage) {
        LOG_ERROR(MAPPER, "unable to allocate PRG storage");
        free(prg_ram_storage);
        prg_ram_storage = NULL;
        return false;
    }
    memcpy(prg_rom_storage, ines_file_data->prg_rom, prg_rom_size_bytes);
    chr_size_bytes = ines_file_data->header.chr_8kb_banks * 8192;
    chr_is_ram = (0 == chr_size_bytes);
    chr_rom_bytes = chr_size_bytes;
    chr_ram_start = chr_size_bytes; // (no RAM)
    if (chr_is_ram) {
        // no CHR-ROM: CHR-RAM (NES 2.0 byte 11, at least 1KB for the bank math; iNES 1.0: 8KB)
        chr_size_bytes = (chr_ram_size < 1024) ? 8 * 1024 : chr_ram_size;
        chr_rom_bytes = chr_size_bytes;
        chr_ram_start = 0;
    } else if (BOARD_TQROM == board) {
        chr_size_bytes += 8 * 1024; // the CHR-ROM, then the CHR-RAM
        chr_ram_start = chr_rom_bytes;
    }
    chr_storage = calloc(1, chr_size_bytes);
    if (NULL == chr_storage) {
        LOG_ERROR(MAPPER, "unable to allocate CHR storage");
        free(prg_rom_storage);
        prg_rom_storage = NULL;
        free(prg_ram_storage);
        prg_ram_storage = NULL;
        return false;
    }
    if (!chr_is_ram) {
        memcpy(chr_storage, ines_file_data->chr_rom, chr_rom_bytes); // (TQROM: chr_size_bytes also holds the CHR-RAM)
    }

    // Four-screen boards (Gauntlet, Rad Racer II) carry their own 4KB of name table RAM; $A000 does nothing there
    four_screen = ((ines_file_data->header.flags_6 & BIT_3) != 0) && (BOARD_TXSROM != board);
    memset(four_screen_ram, 0, sizeof(four_screen_ram));
    // Power-on mirroring from the header (flag 6 bit 0: 1 = vertical) until the game writes $A000
    mmc_regs.nametable_arrangement = (ines_file_data->header.flags_6 & BIT_0) ? 0 : 1;

    memcpy(R, (uint8_t[]){0, 2, 4, 5, 6, 7, 0, 1}, sizeof(R)); // Initial values of R (from mesen)
    mapper_txrom_update_prg_banks();
    mapper_txrom_update_chr_banks();
    mapper_txrom_update_mirroring();

    p_mapper_info->deinit = mapper_txrom_deinit;
    p_mapper_info->read_prg = mapper_txrom_read_prg;
    p_mapper_info->write_prg = mapper_txrom_write_prg;
    p_mapper_info->read_chr = mapper_txrom_read_chr;
    p_mapper_info->write_chr = mapper_txrom_write_chr;
    p_mapper_info->get_ciram_addr = mapper_txrom_get_ciram_addr;
    if (four_screen) {
        p_mapper_info->read_nt = mapper_txrom_read_nt;
        p_mapper_info->write_nt = mapper_txrom_write_nt;
    }
    p_mapper_info->debug_read_chr = mapper_txrom_debug_read_chr;
    p_mapper_info->debug_chr_offset = mapper_txrom_debug_chr_offset;
    p_mapper_info->debug_chr_data = mapper_txrom_debug_chr_data;
    p_mapper_info->debug_prg_offset = mapper_txrom_debug_prg_offset;
    p_mapper_info->prg_bank_size = 0x2000;
    p_mapper_info->debug_prg_ram = mapper_txrom_debug_prg_ram;
    p_mapper_info->get_sav_data = mapper_txrom_get_sav_data;
    p_mapper_info->state = mapper_txrom_state;
    if (!board_is_namco108()) { // (the Namco 108 has no IRQ: nothing watches A12)
        p_mapper_info->ppu_a12 = mapper_txrom_ppu_a12;
        p_mapper_info->a12_rises_to_irq = mapper_txrom_a12_rises_to_irq;
    }
    p_mapper_info->debug_read_prg = mapper_txrom_debug_read_prg;

    return true;
}
