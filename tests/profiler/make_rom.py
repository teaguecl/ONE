#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Writes profiler.nes: the NROM-128 test program of tests/debugger_profiler.txt (hand-assembled, no assembler needed).

The main code waits for two vblanks, enables NMI, then calls
  A ($C100) three times; A calls B ($C110) twice; B is NOP NOP RTS
  R ($C120) once with X = 3: it calls itself until X reaches 0 (three activations)
  W ($C130): waits for the flag at $00 that the NMI handler ($C140: LDA #1, STA $00, RTI) sets
and ends in a JMP to itself at DONE ($C0xx, printed below).
"""
import sys

prg = bytearray([0xFF] * 0x4000)  # mapped at $8000 and $C000


def put(addr, data):
    prg[addr - 0xC000:addr - 0xC000 + len(data)] = bytes(data)


def jsr(a):
    return [0x20, a & 0xFF, a >> 8]


code = [0x78, 0xD8, 0xA2, 0xFF, 0x9A]            # SEI CLD LDX #$FF TXS
code += [0x2C, 0x02, 0x20, 0x10, 0xFB] * 2       # BIT $2002 / BPL back: wait for a vblank, twice
code += [0xA9, 0x00, 0x85, 0x00]                 # LDA #0 STA $00
code += [0xA9, 0x80, 0x8D, 0x00, 0x20]           # LDA #$80 STA $2000: NMI on
code += jsr(0xC100) * 3
code += [0xA2, 0x03] + jsr(0xC120)               # LDX #3, JSR R
code += jsr(0xC130)                              # JSR W
done = 0xC000 + len(code)
code += [0x4C, done & 0xFF, done >> 8]           # DONE: JMP DONE
put(0xC000, code)
put(0xC100, jsr(0xC110) + jsr(0xC110) + [0x60])  # A
put(0xC110, [0xEA, 0xEA, 0x60])                  # B
put(0xC120, [0xCA, 0xF0, 0x03] + jsr(0xC120) + [0x60])  # R: DEX BEQ +3 JSR R RTS
put(0xC130, [0xA5, 0x00, 0xF0, 0xFC, 0x60])      # W: LDA $00 BEQ W RTS
put(0xC140, [0xA9, 0x01, 0x85, 0x00, 0x40])      # NMI: LDA #1 STA $00 RTI
put(0xFFFA, [0x40, 0xC1, 0x00, 0xC0, 0x40, 0xC1])  # NMI, reset, IRQ
header = bytes([0x4E, 0x45, 0x53, 0x1A, 1, 0, 0, 0] + [0] * 8)  # 16 KB PRG, CHR RAM, horizontal
with open(sys.argv[1], "wb") as f:
    f.write(header + bytes(prg))
print("DONE = $%04X" % done)
