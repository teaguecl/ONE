#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Writes lag.nes: the NROM-128 test program of tests/debugger_lag.txt (hand-assembled, no assembler needed).

After two vblank waits it enables NMI and spins. The NMI handler counts frames in $01 and polls controller 1 (strobe,
then a read of $4016) only when the count is not a multiple of 4: every fourth frame is a lag frame, so any 40
frames contain exactly 10 of them whatever the phase.
"""
import sys

prg = bytearray([0xFF] * 0x4000)  # mapped at $8000 and $C000


def put(addr, data):
    prg[addr - 0xC000:addr - 0xC000 + len(data)] = bytes(data)


code = [0x78, 0xD8, 0xA2, 0xFF, 0x9A]            # SEI CLD LDX #$FF TXS
code += [0x2C, 0x02, 0x20, 0x10, 0xFB] * 2       # BIT $2002 / BPL back: wait for a vblank, twice
code += [0xA9, 0x00, 0x85, 0x01]                 # LDA #0 STA $01
code += [0xA9, 0x80, 0x8D, 0x00, 0x20]           # LDA #$80 STA $2000: NMI on
spin = 0xC000 + len(code)
code += [0x4C, spin & 0xFF, spin >> 8]           # JMP spin
put(0xC000, code)
nmi = [0x48,                                     # PHA
       0xE6, 0x01,                               # INC $01
       0xA5, 0x01, 0x29, 0x03,                   # LDA $01 AND #3
       0xF0, 0x0D,                               # BEQ skip (a multiple of 4: no poll)
       0xA9, 0x01, 0x8D, 0x16, 0x40,             # LDA #1 STA $4016
       0xA9, 0x00, 0x8D, 0x16, 0x40,             # LDA #0 STA $4016: the strobe latches the buttons
       0xAD, 0x16, 0x40,                         # LDA $4016
       0x68, 0x40]                               # skip: PLA RTI
put(0xC100, nmi)
put(0xFFFA, [0x00, 0xC1, 0x00, 0xC0, 0x00, 0xC1])  # NMI, reset, IRQ
header = bytes([0x4E, 0x45, 0x53, 0x1A, 1, 0, 0, 0] + [0] * 8)  # 16 KB PRG, CHR RAM, horizontal
with open(sys.argv[1], "wb") as f:
    f.write(header + bytes(prg))
