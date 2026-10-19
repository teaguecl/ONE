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
#ifndef debugger_priv_h_defined
#define debugger_priv_h_defined

#include <stdbool.h>
#include "cpu.h"
#include "bits.h"

typedef enum {
    ADC,
    AND,
    ASL,
    BCC,
    BCS,
    BEQ,
    BIT,
    BMI,
    BNE,
    BPL,
    BRK,
    BVC,
    BVS,
    CLC,
    CLD,
    CLI,
    CLV,
    CMP,
    CPX,
    CPY,
    DCP,
    DEC,
    DEX,
    DEY,
    EOR,
    INC,
    INX,
    INY,
    ISB,
    JMP,
    JSR,
    LAX,
    LDA,
    LDX,
    LDY,
    LSR,
    NOP,
    ORA,
    PHA,
    PHP,
    PLA,
    PLP,
    RLA,
    ROL,
    ROR,
    RRA,
    RTI,
    RTS,
    SAX,
    SBC,
    SEC,
    SED,
    SEI,
    SHY,
    SLO,
    SRE,
    STA,
    STX,
    STY,
    TAX,
    TAY,
    TSX,
    TXA,
    TXS,
    TYA,
    ILL,
    /* unofficial opcodes added later (appended so existing values don't move) */
    ANC,
    ALR,
    ARR,
    SBX,
    SHX,
    SHA,
    TAS,
    LAS,
    XAA /* printed as ANE (Mesen) */,
    STP,
    NUM_MNEMONICS
} mnemonic_type;

typedef enum {
    IMPLIED,
    ABSOLUTE,
    ABSOLUTE_X,
    ABSOLUTE_Y,
    ZERO_PAGE,
    ZERO_PAGE_X,
    ZERO_PAGE_Y,
    IMMEDIATE,
    RELATIVE,
    INDIRECT,
    INDIRECT_X,
    INDIRECT_Y,
    ACCUMULATOR,
    NUM_ADDR_MODES

} addressing_mode;

typedef struct {
    mnemonic_type type;
    addressing_mode mode;
    bool is_documented;
} instruction_data;

// 6502 opcode tables (defined in trace.c; shared by the trace and the debugger's disassembler)
extern const uint8_t addressing_mode_sizes[NUM_ADDR_MODES];
extern const char *mnemonic_strings[NUM_MNEMONICS];
extern const instruction_data instruction_info[0x100];

#define ABS_GET_SRC_ADDR(lsb, msb) ((((msb) << 8) | (lsb)))
#define ABS_GET_SRC_DATA(lsb, msb) (cpu_debug_read(ABS_GET_SRC_ADDR(lsb, msb)))
#define IMM_GET_SRC_DATA(lsb)      (lsb)
#define ZP_GET_SRC_DATA(lsb)       (cpu_debug_read(lsb))

#define IND_GET_SRC_ADDR(lsb, msb) (cpu_debug_read_word((((msb) << 8) | (lsb))))
#define IND_GET_SRC_DATA(lsb, msb) (cpu_debug_read_word(IND_GET_SRC_ADDR(lsb, msb)))

#define INDX_GET_SRC_ADDR(lsb)      (cpu_debug_read_word((lsb) + cpu_debug_get_reg_x()))
#define INDX_GET_SRC_DATA(lsb)      (cpu_debug_read(INDX_GET_SRC_ADDR(lsb)))
#define INDY_GET_SRC_ADDR(lsb)      (cpu_debug_read_word(lsb) + cpu_debug_get_reg_y())
#define INDY_GET_SRC_DATA(lsb)      (cpu_debug_read(INDY_GET_SRC_ADDR(lsb)))
#define ZPX_GET_SRC_ADDR(lsb)       (cpu_debug_read(lsb) + cpu_debug_get_reg_x())
#define ZPX_GET_SRC_DATA(lsb)       (cpu_debug_read(ZPX_GET_SRC_ADDR(lsb)))
#define ZPY_GET_SRC_ADDR(lsb)       (cpu_debug_read(lsb) + cpu_debug_get_reg_y())
#define ZPY_GET_SRC_DATA(lsb)       (cpu_debug_read(ZPY_GET_SRC_ADDR(lsb)))
#define ABSX_GET_SRC_ADDR(lsb, msb) ((((msb) << 8) | (lsb)) + cpu_debug_get_reg_x())
#define ABSX_GET_SRC_DATA(lsb, msb) (cpu_debug_read(ABSX_GET_SRC_ADDR(lsb, msb)))
#define ABSY_GET_SRC_ADDR(lsb, msb) ((((msb) << 8) | (lsb)) + cpu_debug_get_reg_y())
#define ABSY_GET_SRC_DATA(lsb, msb) (cpu_debug_read(ABSY_GET_SRC_ADDR(lsb, msb)))

#endif
