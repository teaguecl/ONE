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
/* trace.c
 *
 * Standalone Mesen-compatible CPU trace logger, extracted from
 * debugger.c's debugger_generate_mesen_trace() + its three opcode
 * metadata tables. The original lives inside a translation unit that
 * pulls in ncurses and SDL for the interactive debugger UI; this
 * function itself touches neither, so it's split out here to make
 * Mesen-style tracing available in the normal (non-debug) build too.
 *
 * Requires TRACE_TYPE_MESEN, defined below, to select the Mesen-style
 * "[$addr],Y [$resolved] = $val" operand format over the Nintendulator
 * variant also present in the original source.
 */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdbool.h>
#include "cpu.h"
#include "ppu.h"
#include "debugger_priv.h"
#include "trace.h"

static FILE *trace_out = NULL; // NULL: stderr

void trace_set_file(FILE *file)
{
    trace_out = file;
}

#define TRACE_TYPE_MESEN

const uint8_t addressing_mode_sizes[NUM_ADDR_MODES] = {
    1, /* implied */
    3, /* absolute */
    3, /* absolute X */
    3, /* absolute Y */
    2, /* zero page */
    2, /* zero page X */
    2, /* zero page Y */
    2, /* immediate */
    2, /* relative */
    3, /* indirect */
    2, /* indirect X */
    2, /* indirect Y */
    1, /* accumulator */
};

const char *mnemonic_strings[NUM_MNEMONICS] = {
    "ADC", "AND", "ASL", "BCC", "BCS", "BEQ", "BIT", "BMI", "BNE", "BPL", "BRK", "BVC", "BVS", "CLC", "CLD", "CLI",
    "CLV", "CMP", "CPX", "CPY", "DCP", "DEC", "DEX", "DEY", "EOR", "INC", "INX", "INY",
#ifdef TRACE_TYPE_MESEN
    "ISC", /* Mesen's name for ISB */
#else
    "ISB",
#endif
    "JMP", "JSR", "LAX", "LDA", "LDX", "LDY", "LSR", "NOP", "ORA", "PHA", "PHP", "PLA", "PLP", "RLA", "ROL", "ROR",
    "RRA", "RTI", "RTS", "SAX", "SBC", "SEC", "SED", "SEI", "SHY", "SLO", "SRE", "STA", "STX", "STY", "TAX", "TAY",
    "TSX", "TXA", "TXS", "TYA", "ILL", "ANC", "ALR", "ARR", "SBX", "SHX", "SHA", "TAS", "LAS", "ANE", "STP"};

const instruction_data instruction_info[0x100] = {
    {BRK, IMPLIED, true},      /* opcode 0x00 */
    {ORA, INDIRECT_X, true},   /* opcode 0x01 */
    {STP, IMPLIED, false},     /* opcode 0x02 */
    {SLO, INDIRECT_X, false},  /* opcode 0x03 */
    {NOP, ZERO_PAGE, false},   /* opcode 0x04 */
    {ORA, ZERO_PAGE, true},    /* opcode 0x05 */
    {ASL, ZERO_PAGE, true},    /* opcode 0x06 */
    {SLO, ZERO_PAGE, false},   /* opcode 0x07 */
    {PHP, IMPLIED, true},      /* opcode 0x08 */
    {ORA, IMMEDIATE, true},    /* opcode 0x09 */
    {ASL, ACCUMULATOR, true},  /* opcode 0x0A */
    {ANC, IMMEDIATE, false},   /* opcode 0x0B */
    {NOP, ABSOLUTE, false},    /* opcode 0x0C */
    {ORA, ABSOLUTE, true},     /* opcode 0x0D */
    {ASL, ABSOLUTE, true},     /* opcode 0x0E */
    {SLO, ABSOLUTE, false},    /* opcode 0x0F */
    {BPL, RELATIVE, true},     /* opcode 0x10 */
    {ORA, INDIRECT_Y, true},   /* opcode 0x11 */
    {STP, IMPLIED, false},     /* opcode 0x12 */
    {SLO, INDIRECT_Y, false},  /* opcode 0x13 */
    {NOP, ZERO_PAGE_X, false}, /* opcode 0x14 */
    {ORA, ZERO_PAGE_X, true},  /* opcode 0x15 */
    {ASL, ZERO_PAGE_X, true},  /* opcode 0x16 */
    {SLO, ZERO_PAGE_X, false}, /* opcode 0x17 */
    {CLC, IMPLIED, true},      /* opcode 0x18 */
    {ORA, ABSOLUTE_Y, true},   /* opcode 0x19 */
    {NOP, IMPLIED, false},     /* opcode 0x1A */
    {SLO, ABSOLUTE_Y, false},  /* opcode 0x1B */
    {NOP, ABSOLUTE_X, false},  /* opcode 0x1C */
    {ORA, ABSOLUTE_X, true},   /* opcode 0x1D */
    {ASL, ABSOLUTE_X, true},   /* opcode 0x1E */
    {SLO, ABSOLUTE_X, false},  /* opcode 0x1F */
    {JSR, ABSOLUTE, true},     /* opcode 0x20 */
    {AND, INDIRECT_X, true},   /* opcode 0x21 */
    {STP, IMPLIED, false},     /* opcode 0x22 */
    {RLA, INDIRECT_X, false},  /* opcode 0x23 */
    {BIT, ZERO_PAGE, true},    /* opcode 0x24 */
    {AND, ZERO_PAGE, true},    /* opcode 0x25 */
    {ROL, ZERO_PAGE, true},    /* opcode 0x26 */
    {RLA, ZERO_PAGE, false},   /* opcode 0x27 */
    {PLP, IMPLIED, true},      /* opcode 0x28 */
    {AND, IMMEDIATE, true},    /* opcode 0x29 */
    {ROL, ACCUMULATOR, true},  /* opcode 0x2A */
    {ANC, IMMEDIATE, false},   /* opcode 0x2B */
    {BIT, ABSOLUTE, true},     /* opcode 0x2C */
    {AND, ABSOLUTE, true},     /* opcode 0x2D */
    {ROL, ABSOLUTE, true},     /* opcode 0x2E */
    {RLA, ABSOLUTE, false},    /* opcode 0x2F */
    {BMI, RELATIVE, true},     /* opcode 0x30 */
    {AND, INDIRECT_Y, true},   /* opcode 0x31 */
    {STP, IMPLIED, false},     /* opcode 0x32 */
    {RLA, INDIRECT_Y, false},  /* opcode 0x33 */
    {NOP, ZERO_PAGE_X, false}, /* opcode 0x34 */
    {AND, ZERO_PAGE_X, true},  /* opcode 0x35 */
    {ROL, ZERO_PAGE_X, true},  /* opcode 0x36 */
    {RLA, ZERO_PAGE_X, false}, /* opcode 0x37 */
    {SEC, IMPLIED, true},      /* opcode 0x38 */
    {AND, ABSOLUTE_Y, true},   /* opcode 0x39 */
    {NOP, IMPLIED, false},     /* opcode 0x3A */
    {RLA, ABSOLUTE_Y, false},  /* opcode 0x3B */
    {NOP, ABSOLUTE_X, false},  /* opcode 0x3C */
    {AND, ABSOLUTE_X, true},   /* opcode 0x3D */
    {ROL, ABSOLUTE_X, true},   /* opcode 0x3E */
    {RLA, ABSOLUTE_X, false},  /* opcode 0x3F */
    {RTI, IMPLIED, true},      /* opcode 0x40 */
    {EOR, INDIRECT_X, true},   /* opcode 0x41 */
    {STP, IMPLIED, false},     /* opcode 0x42 */
    {SRE, INDIRECT_X, false},  /* opcode 0x43 */
    {NOP, ZERO_PAGE, false},   /* opcode 0x44 */
    {EOR, ZERO_PAGE, true},    /* opcode 0x45 */
    {LSR, ZERO_PAGE, true},    /* opcode 0x46 */
    {SRE, ZERO_PAGE, false},   /* opcode 0x47 */
    {PHA, IMPLIED, true},      /* opcode 0x48 */
    {EOR, IMMEDIATE, true},    /* opcode 0x49 */
    {LSR, ACCUMULATOR, true},  /* opcode 0x4A */
    {ALR, IMMEDIATE, false},   /* opcode 0x4B */
    {JMP, ABSOLUTE, true},     /* opcode 0x4C */
    {EOR, ABSOLUTE, true},     /* opcode 0x4D */
    {LSR, ABSOLUTE, true},     /* opcode 0x4E */
    {SRE, ABSOLUTE, false},    /* opcode 0x4F */
    {BVC, RELATIVE, true},     /* opcode 0x50 */
    {EOR, INDIRECT_Y, true},   /* opcode 0x51 */
    {STP, IMPLIED, false},     /* opcode 0x52 */
    {SRE, INDIRECT_Y, false},  /* opcode 0x53 */
    {NOP, ZERO_PAGE_X, false}, /* opcode 0x54 */
    {EOR, ZERO_PAGE_X, true},  /* opcode 0x55 */
    {LSR, ZERO_PAGE_X, true},  /* opcode 0x56 */
    {SRE, ZERO_PAGE_X, false}, /* opcode 0x57 */
    {CLI, IMPLIED, true},      /* opcode 0x58 */
    {EOR, ABSOLUTE_Y, true},   /* opcode 0x59 */
    {NOP, IMPLIED, false},     /* opcode 0x5A */
    {SRE, ABSOLUTE_Y, false},  /* opcode 0x5B */
    {NOP, ABSOLUTE_X, false},  /* opcode 0x5C */
    {EOR, ABSOLUTE_X, true},   /* opcode 0x5D */
    {LSR, ABSOLUTE_X, true},   /* opcode 0x5E */
    {SRE, ABSOLUTE_X, false},  /* opcode 0x5F */
    {RTS, IMPLIED, true},      /* opcode 0x60 */
    {ADC, INDIRECT_X, true},   /* opcode 0x61 */
    {STP, IMPLIED, false},     /* opcode 0x62 */
    {RRA, INDIRECT_X, false},  /* opcode 0x63 */
    {NOP, ZERO_PAGE, false},   /* opcode 0x64 */
    {ADC, ZERO_PAGE, true},    /* opcode 0x65 */
    {ROR, ZERO_PAGE, true},    /* opcode 0x66 */
    {RRA, ZERO_PAGE, false},   /* opcode 0x67 */
    {PLA, IMPLIED, true},      /* opcode 0x68 */
    {ADC, IMMEDIATE, true},    /* opcode 0x69 */
    {ROR, ACCUMULATOR, true},  /* opcode 0x6A */
    {ARR, IMMEDIATE, false},   /* opcode 0x6B */
    {JMP, INDIRECT, true},     /* opcode 0x6C */
    {ADC, ABSOLUTE, true},     /* opcode 0x6D */
    {ROR, ABSOLUTE, true},     /* opcode 0x6E */
    {RRA, ABSOLUTE, false},    /* opcode 0x6F */
    {BVS, RELATIVE, true},     /* opcode 0x70 */
    {ADC, INDIRECT_Y, true},   /* opcode 0x71 */
    {STP, IMPLIED, false},     /* opcode 0x72 */
    {RRA, INDIRECT_Y, false},  /* opcode 0x73 */
    {NOP, ZERO_PAGE_X, false}, /* opcode 0x74 */
    {ADC, ZERO_PAGE_X, true},  /* opcode 0x75 */
    {ROR, ZERO_PAGE_X, true},  /* opcode 0x76 */
    {RRA, ZERO_PAGE_X, false}, /* opcode 0x77 */
    {SEI, IMPLIED, true},      /* opcode 0x78 */
    {ADC, ABSOLUTE_Y, true},   /* opcode 0x79 */
    {NOP, IMPLIED, false},     /* opcode 0x7A */
    {RRA, ABSOLUTE_Y, false},  /* opcode 0x7B */
    {NOP, ABSOLUTE_X, false},  /* opcode 0x7C */
    {ADC, ABSOLUTE_X, true},   /* opcode 0x7D */
    {ROR, ABSOLUTE_X, true},   /* opcode 0x7E */
    {RRA, ABSOLUTE_X, false},  /* opcode 0x7F */
    {NOP, IMMEDIATE, false},   /* opcode 0x80 */
    {STA, INDIRECT_X, true},   /* opcode 0x81 */
    {NOP, IMMEDIATE, false},   /* opcode 0x82 */
    {SAX, INDIRECT_X, false},  /* opcode 0x83 */
    {STY, ZERO_PAGE, true},    /* opcode 0x84 */
    {STA, ZERO_PAGE, true},    /* opcode 0x85 */
    {STX, ZERO_PAGE, true},    /* opcode 0x86 */
    {SAX, ZERO_PAGE, false},   /* opcode 0x87 */
    {DEY, IMPLIED, true},      /* opcode 0x88 */
    {NOP, IMMEDIATE, false},   /* opcode 0x89 */
    {TXA, IMPLIED, true},      /* opcode 0x8A */
    {XAA, IMMEDIATE, false},   /* opcode 0x8B */
    {STY, ABSOLUTE, true},     /* opcode 0x8C */
    {STA, ABSOLUTE, true},     /* opcode 0x8D */
    {STX, ABSOLUTE, true},     /* opcode 0x8E */
    {SAX, ABSOLUTE, false},    /* opcode 0x8F */
    {BCC, RELATIVE, true},     /* opcode 0x90 */
    {STA, INDIRECT_Y, true},   /* opcode 0x91 */
    {STP, IMPLIED, false},     /* opcode 0x92 */
    {SHA, INDIRECT_Y, false},  /* opcode 0x93 */
    {STY, ZERO_PAGE_X, true},  /* opcode 0x94 */
    {STA, ZERO_PAGE_X, true},  /* opcode 0x95 */
    {STX, ZERO_PAGE_Y, true},  /* opcode 0x96 */
    {SAX, ZERO_PAGE_Y, false}, /* opcode 0x97 */
    {TYA, IMPLIED, true},      /* opcode 0x98 */
    {STA, ABSOLUTE_Y, true},   /* opcode 0x99 */
    {TXS, IMPLIED, true},      /* opcode 0x9A */
    {TAS, ABSOLUTE_Y, false},  /* opcode 0x9B */
    {SHY, ABSOLUTE_X, false},  /* opcode 0x9C */
    {STA, ABSOLUTE_X, true},   /* opcode 0x9D */
    {SHX, ABSOLUTE_Y, false},  /* opcode 0x9E */
    {SHA, ABSOLUTE_Y, false},  /* opcode 0x9F */
    {LDY, IMMEDIATE, true},    /* opcode 0xA0 */
    {LDA, INDIRECT_X, true},   /* opcode 0xA1 */
    {LDX, IMMEDIATE, true},    /* opcode 0xA2 */
    {LAX, INDIRECT_X, false},  /* opcode 0xA3 */
    {LDY, ZERO_PAGE, true},    /* opcode 0xA4 */
    {LDA, ZERO_PAGE, true},    /* opcode 0xA5 */
    {LDX, ZERO_PAGE, true},    /* opcode 0xA6 */
    {LAX, ZERO_PAGE, false},   /* opcode 0xA7 */
    {TAY, IMPLIED, true},      /* opcode 0xA8 */
    {LDA, IMMEDIATE, true},    /* opcode 0xA9 */
    {TAX, IMPLIED, true},      /* opcode 0xAA */
    {LAX, IMMEDIATE, false},   /* opcode 0xAB */
    {LDY, ABSOLUTE, true},     /* opcode 0xAC */
    {LDA, ABSOLUTE, true},     /* opcode 0xAD */
    {LDX, ABSOLUTE, true},     /* opcode 0xAE */
    {LAX, ABSOLUTE, false},    /* opcode 0xAF */
    {BCS, RELATIVE, true},     /* opcode 0xB0 */
    {LDA, INDIRECT_Y, true},   /* opcode 0xB1 */
    {STP, IMPLIED, false},     /* opcode 0xB2 */
    {LAX, INDIRECT_Y, false},  /* opcode 0xB3 */
    {LDY, ZERO_PAGE_X, true},  /* opcode 0xB4 */
    {LDA, ZERO_PAGE_X, true},  /* opcode 0xB5 */
    {LDX, ZERO_PAGE_Y, true},  /* opcode 0xB6 */
    {LAX, ZERO_PAGE_Y, false}, /* opcode 0xB7 */
    {CLV, IMPLIED, true},      /* opcode 0xB8 */
    {LDA, ABSOLUTE_Y, true},   /* opcode 0xB9 */
    {TSX, IMPLIED, true},      /* opcode 0xBA */
    {LAS, ABSOLUTE_Y, false},  /* opcode 0xBB */
    {LDY, ABSOLUTE_X, true},   /* opcode 0xBC */
    {LDA, ABSOLUTE_X, true},   /* opcode 0xBD */
    {LDX, ABSOLUTE_Y, true},   /* opcode 0xBE */
    {LAX, ABSOLUTE_Y, false},  /* opcode 0xBF */
    {CPY, IMMEDIATE, true},    /* opcode 0xC0 */
    {CMP, INDIRECT_X, true},   /* opcode 0xC1 */
    {NOP, IMMEDIATE, false},   /* opcode 0xC2 */
    {DCP, INDIRECT_X, false},  /* opcode 0xC3 */
    {CPY, ZERO_PAGE, true},    /* opcode 0xC4 */
    {CMP, ZERO_PAGE, true},    /* opcode 0xC5 */
    {DEC, ZERO_PAGE, true},    /* opcode 0xC6 */
    {DCP, ZERO_PAGE, false},   /* opcode 0xC7 */
    {INY, IMPLIED, true},      /* opcode 0xC8 */
    {CMP, IMMEDIATE, true},    /* opcode 0xC9 */
    {DEX, IMPLIED, true},      /* opcode 0xCA */
    {SBX, IMMEDIATE, false},   /* opcode 0xCB */
    {CPY, ABSOLUTE, true},     /* opcode 0xCC */
    {CMP, ABSOLUTE, true},     /* opcode 0xCD */
    {DEC, ABSOLUTE, true},     /* opcode 0xCE */
    {DCP, ABSOLUTE, false},    /* opcode 0xCF */
    {BNE, RELATIVE, true},     /* opcode 0xD0 */
    {CMP, INDIRECT_Y, true},   /* opcode 0xD1 */
    {STP, IMPLIED, false},     /* opcode 0xD2 */
    {DCP, INDIRECT_Y, false},  /* opcode 0xD3 */
    {NOP, ZERO_PAGE_X, false}, /* opcode 0xD4 */
    {CMP, ZERO_PAGE_X, true},  /* opcode 0xD5 */
    {DEC, ZERO_PAGE_X, true},  /* opcode 0xD6 */
    {DCP, ZERO_PAGE_X, false}, /* opcode 0xD7 */
    {CLD, IMPLIED, true},      /* opcode 0xD8 */
    {CMP, ABSOLUTE_Y, true},   /* opcode 0xD9 */
    {NOP, IMPLIED, false},     /* opcode 0xDA */
    {DCP, ABSOLUTE_Y, false},  /* opcode 0xDB */
    {NOP, ABSOLUTE_X, false},  /* opcode 0xDC */
    {CMP, ABSOLUTE_X, true},   /* opcode 0xDD */
    {DEC, ABSOLUTE_X, true},   /* opcode 0xDE */
    {DCP, ABSOLUTE_X, false},  /* opcode 0xDF */
    {CPX, IMMEDIATE, true},    /* opcode 0xE0 */
    {SBC, INDIRECT_X, true},   /* opcode 0xE1 */
    {NOP, IMMEDIATE, false},   /* opcode 0xE2 */
    {ISB, INDIRECT_X, false},  /* opcode 0xE3 */
    {CPX, ZERO_PAGE, true},    /* opcode 0xE4 */
    {SBC, ZERO_PAGE, true},    /* opcode 0xE5 */
    {INC, ZERO_PAGE, true},    /* opcode 0xE6 */
    {ISB, ZERO_PAGE, false},   /* opcode 0xE7 */
    {INX, IMPLIED, true},      /* opcode 0xE8 */
    {SBC, IMMEDIATE, true},    /* opcode 0xE9 */
    {NOP, IMPLIED, true},      /* opcode 0xEA */
    {SBC, IMMEDIATE, false},   /* opcode 0xEB */
    {CPX, ABSOLUTE, true},     /* opcode 0xEC */
    {SBC, ABSOLUTE, true},     /* opcode 0xED */
    {INC, ABSOLUTE, true},     /* opcode 0xEE */
    {ISB, ABSOLUTE, false},    /* opcode 0xEF */
    {BEQ, RELATIVE, true},     /* opcode 0xF0 */
    {SBC, INDIRECT_Y, true},   /* opcode 0xF1 */
    {STP, IMPLIED, false},     /* opcode 0xF2 */
    {ISB, INDIRECT_Y, false},  /* opcode 0xF3 */
    {NOP, ZERO_PAGE_X, false}, /* opcode 0xF4 */
    {SBC, ZERO_PAGE_X, true},  /* opcode 0xF5 */
    {INC, ZERO_PAGE_X, true},  /* opcode 0xF6 */
    {ISB, ZERO_PAGE_X, false}, /* opcode 0xF7 */
    {SED, IMPLIED, true},      /* opcode 0xF8 */
    {SBC, ABSOLUTE_Y, true},   /* opcode 0xF9 */
    {NOP, IMPLIED, false},     /* opcode 0xFA */
    {ISB, ABSOLUTE_Y, false},  /* opcode 0xFB */
    {NOP, ABSOLUTE_X, false},  /* opcode 0xFC */
    {SBC, ABSOLUTE_X, true},   /* opcode 0xFD */
    {INC, ABSOLUTE_X, true},   /* opcode 0xFE */
    {ISB, ABSOLUTE_X, false}   /* opcode 0xFF */
};

// Mesen compatible trace log entry
void debugger_generate_trace(void)
{
    //E242 $20 $DC $E1 JSR $E1DC                       A:00 X:00 Y:00 P:04 SP:FD CYC:30  SL:0   CPU Cycle:0
    char trace_log[160] = {0};
    char temp_buf[96];
    const char *spaces = "                                       ";
    uint16_t pc;
    uint8_t opcode;
    uint16_t addr_mode;
    uint8_t instruction_size;
    uint16_t mnemonic;
    int i;
    uint8_t data1, data2;
    int scanline;

    pc = cpu_debug_get_pc();
    opcode = cpu_debug_read(pc);
    addr_mode = instruction_info[opcode].mode;
    instruction_size = addressing_mode_sizes[addr_mode];
    mnemonic = instruction_info[opcode].type;
    int dot;
    ppu_debug_get_trace_position(&scanline, &dot);

    // PC field
    sprintf(trace_log, "%04X  ", pc);


    //    strncat(trace_log, spaces, (17-strlen(trace_log)));

    /* add mnemonic name */
    strcat(trace_log, mnemonic_strings[mnemonic]);

#ifndef TRACE_TYPE_MESEN
    /* add "*" if it is a undocumented opcode (Nintendulator style; Mesen doesn't mark them) */
    if (false == instruction_info[opcode].is_documented) {
        strcat(trace_log, "*");
    }
#endif

    /* add decoded data */
    switch (addr_mode) {
    case IMPLIED: break;
    case ABSOLUTE: {
        data1 = cpu_debug_read(pc + 1);
        data2 = cpu_debug_read(pc + 2);

        sprintf(temp_buf, " $%02X%02X", data2, data1);
        strcat(trace_log, temp_buf);
        if ((opcode == 0x0d) || (opcode == 0x8d) || (opcode == 0x8e) || (opcode == 0xad) || (opcode == 0x2c) ||
            (opcode == 0xae) || (opcode == 0xac) || (opcode == 0x8c) || (opcode == 0x2d) || (opcode == 0x4d) ||
            (opcode == 0x6d) || (opcode == 0xcd) || (opcode == 0xed) || (opcode == 0xec) || (opcode == 0xcc) ||
            (opcode == 0x4e) || (opcode == 0x0e) || (opcode == 0x6e) || (opcode == 0x2e) || (opcode == 0xee) ||
            (opcode == 0xce) || (opcode == 0x0c) || (opcode == 0xaf) || (opcode == 0xcf) || (opcode == 0x8f) ||
            (opcode == 0xef) || (opcode == 0x0f) || (opcode == 0x2f) || (opcode == 0x4f) || (opcode == 0x6f)) {
            uint16_t addr = (uint16_t)data1 | (uint16_t)(data2) << 8;
            sprintf(temp_buf, " = $%02X", cpu_debug_read(addr));
            strcat(trace_log, temp_buf);
        }
    } break;
    case ABSOLUTE_X: {
        data1 = cpu_debug_read(pc + 1);
        data2 = cpu_debug_read(pc + 2);
        uint16_t addr = (uint16_t)data1 | (uint16_t)(data2) << 8;
        uint16_t addr_offset = addr + cpu_debug_get_reg_x();

        //sprintf(temp_buf, " $%04X,X @ $%04X", addr, addr_offset);
        sprintf(temp_buf, " $%04X,X [$%04X] = $%02X", addr, addr_offset, cpu_debug_read(addr_offset));
        strcat(trace_log, temp_buf);
    } break;
    case ABSOLUTE_Y: {
        data1 = cpu_debug_read(pc + 1);
        data2 = cpu_debug_read(pc + 2);
        uint16_t addr = (uint16_t)data1 | (uint16_t)(data2) << 8;
        uint16_t addr_offset = addr + cpu_debug_get_reg_y();

        //sprintf(temp_buf, " $%04X,Y @ $%04X", addr, addr_offset);
        sprintf(temp_buf, " $%04X,Y [$%04X] = $%02X", addr, addr_offset, cpu_debug_read(addr_offset));
        strcat(trace_log, temp_buf);
    } break;
    case ZERO_PAGE:
        data1 = cpu_debug_read(pc + 1);
        sprintf(temp_buf, " $%02X = $%02X", data1, cpu_debug_read(data1));
        //sprintf(temp_buf, " $%02X", data1);
        strcat(trace_log, temp_buf);
        break;
    case ZERO_PAGE_X:
        data1 = cpu_debug_read(pc + 1);
        data2 = data1 + cpu_debug_get_reg_x();
        sprintf(temp_buf, " $%02X,X [$%04X] = $%02X", data1, data2, cpu_debug_read(data2));
        //sprintf(temp_buf, " $%02X,X @ $%02X", data1, data2);
        strcat(trace_log, temp_buf);
        break;
    case ZERO_PAGE_Y:
        data1 = cpu_debug_read(pc + 1);
        data2 = data1 + cpu_debug_get_reg_y();
        //sprintf(temp_buf, " $%02X,Y @ %02X", data1, data2);
        sprintf(temp_buf, " $%02X,Y [$%04X] = $%02X", data1, data2, cpu_debug_read(data2));
        strcat(trace_log, temp_buf);
        break;
    case IMMEDIATE:
        sprintf(temp_buf, " #$%02X", cpu_debug_read(pc + 1));
        strcat(trace_log, temp_buf);
        break;
    case RELATIVE: {
        int8_t branch_offset = (int8_t)cpu_debug_read(pc + 1);
        uint16_t target_addr = pc + 2 + branch_offset;
        //sprintf(temp_buf, " $%04X = $%02X", target_addr, cpu_debug_read(target_addr));
        sprintf(temp_buf, " $%04X", target_addr);
        strcat(trace_log, temp_buf);
    } break;
    case INDIRECT: {
        data1 = cpu_debug_read(pc + 1);
        data2 = cpu_debug_read(pc + 2);
        uint16_t addr = (uint16_t)data1 | (uint16_t)(data2) << 8;
        // 6502 JMP ($xxFF) bug: the high byte comes from $xx00, not the next page
        uint16_t target_addr =
            (uint16_t)cpu_debug_read(addr) | ((uint16_t)cpu_debug_read((addr & 0xFF00) | ((addr + 1) & 0x00FF)) << 8);
        sprintf(temp_buf, " ($%04X) [$%04X] = $%02X", addr, target_addr, cpu_debug_read(target_addr));
        strcat(trace_log, temp_buf);
    } break;
    case INDIRECT_X: {
        uint16_t addr;
        data1 = cpu_debug_read(pc + 1);
        addr = cpu_debug_read_word_zp(data1 + cpu_debug_get_reg_x());
        sprintf(temp_buf, " ($%02X,X) [$%04X] = $%02X", data1, addr, cpu_debug_read(addr));
        strcat(trace_log, temp_buf);
    } break;
    case INDIRECT_Y: {
        uint16_t addr;
        uint16_t addr_plus_y;

        data1 = cpu_debug_read(pc + 1);
        addr = cpu_debug_read_word(data1);
        addr_plus_y = addr + cpu_debug_get_reg_y();

#ifdef TRACE_TYPE_NINTENDULATOR
        uint8_t value = 0xA5;
        value = cpu_debug_read(addr_plus_y);
        sprintf(temp_buf, " ($%02X),Y @ $%04X = $%02X", data1, addr_plus_y, value);
#elif defined(TRACE_TYPE_MESEN)
        sprintf(temp_buf, " ($%02X),Y [$%04X] = $%02X", data1, addr_plus_y, cpu_debug_read(addr_plus_y));
#endif
        strcat(trace_log, temp_buf);
    } break;
    case ACCUMULATOR:
        sprintf(temp_buf, " A");
        strcat(trace_log, temp_buf);
        break;
    default: break;
    }

    // registers begin at column 50
    {
        size_t len = strlen(trace_log);
        // pad to 39 columns (a longer operand string gets the full 39 spaces, as the old strncat did)
        size_t pad = (len < 39) ? (39 - len) : strlen(spaces);
        memset(trace_log + len, ' ', pad);
        trace_log[len + pad] = '\0';
    }


    sprintf(temp_buf, "A:%02X X:%02X Y:%02X S:%02X P:%02X V:%-3d H:%-3d Cycle:%" PRIu64 " BC:", cpu_debug_get_reg_a(),
            cpu_debug_get_reg_x(), cpu_debug_get_reg_y(), cpu_debug_get_sp(), cpu_debug_get_reg_p(), scanline, dot,
            cpu_debug_get_cycle());
    strcat(trace_log, temp_buf);

    // opcode and instruction bytes
    sprintf(temp_buf, "%02X", cpu_debug_read(pc));
    strcat(trace_log, temp_buf);
    for (i = 1; i < instruction_size; i++) {
        sprintf(temp_buf, " %02X", cpu_debug_read(pc + i));
        strcat(trace_log, temp_buf);
    }


    fprintf(trace_out != NULL ? trace_out : stderr, "%s\n", trace_log);
}
