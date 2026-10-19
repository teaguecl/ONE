#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Recover the controller input of a Mesen trace log, as an input script for `one --input FILE`.

Usage:
    regression/input_from_log.py LOG [-o FILE]

The trace shows every controller access: a strobe (a write to $4016 with bit 0 = 1, then one with bit 0 = 0) latches
the buttons, and the reads of $4016 / $4017 after it return them one at a time (A, B, Select, Start, Up, Down,
Left, Right) in bit 0 -- the value is in the register of the next trace line. From those reads this rebuilds the
buttons the game saw and writes a line wherever they change:

    CYCLE BUTTONS [BUTTONS2]      # from CPU cycle CYCLE on: controller 1 (and 2) hold these ('-' = none)
    742270 start
    2612502 -

Mesen takes new input once a frame, at the start of vblank (scanline 241), so every strobe of a frame sees the same
buttons. A single strobe can still read something else: a DMC DMA that lands on a $4016 read clocks the controller
once more and shifts the bits (read_joy3 provokes that on purpose; ONE emulates it). So the strobes are grouped by
frame and each frame takes the buttons most of its strobes read (a tie keeps the previous buttons); a change is
written one cycle before the frame's first latch (a change AT the latch cycle comes after it in ONE) (the write that ends a strobe: the controller latches the buttons
then, so a strobe that starts before scanline 241 and ends after it belongs to the new frame).

A DMC DMA that lands on a controller read (the LDA takes more than 4 cycles) clocks the controller once more: the later reads of
that port are one bit early and the 8th is a 1 ("Right" held). The script puts the bits back (the consumed one as not pressed).

Buttons a game never reads (fewer than 8 reads after a strobe) count as up; in a frame with several strobes, those
that read all eight buttons of a port decide for it (a game may strobe once for each port). Reads by instructions other than
LDA/LDX/LDY (whose result is in the next line) and AND with bit 0 of A set (bit 0 of the next line's A) are reported and
skipped.
"""
import argparse
import re
import sys
from collections import Counter

BUTTONS = ["a", "b", "select", "start", "up", "down", "left", "right"]
LINE = re.compile(r'^([0-9A-F]{4})  (\w{3})(.*?)\s+A:([0-9A-F]{2}) X:([0-9A-F]{2}) Y:([0-9A-F]{2}) '
                  r'S:[0-9A-F]{2} P:[0-9A-F]{2} V:(-?\d+)\s+H:\d+\s+Cycle:(\d+)')
LOADS = {"LDA": 4, "LDX": 5, "LDY": 6}  # register group in LINE
STORES = {"STA": 4, "STX": 5, "STY": 6}
VBLANK_LINE = 241  # Mesen updates the input here


def target(operand):
    """The address an instruction accesses: $xxxx, or [$xxxx] for indexed / indirect modes"""
    m = re.search(r'\[\$([0-9A-F]{4})\]', operand) or re.match(r'\s*\$([0-9A-F]{4})\b', operand)
    return int(m.group(1), 16) if m else None


def names(state):
    return "+".join(b for i, b in enumerate(BUTTONS) if state & (1 << i)) or "-"


def read_strobes(path):
    """[(frame, cycle, (port 1 buttons, port 2 buttons))] for every strobe of the log, and the skipped reads"""
    strobes = []
    strobe_high = False
    reads = [0, 0]  # reads of each port since the strobe
    current = None  # [frame, cycle, [bits, bits], [reads, reads], [seen, seen], [dma, dma]] of the latest strobe (frame and cycle of its latch)
    pending_read = None
    load_cycle = 0
    load_direct = False
    skipped = 0
    frame = 0
    previous_line = None
    with open(path, encoding="latin-1") as log:
        for line in log:
            m = LINE.match(line)
            if not m:
                continue
            _, mnemonic, operand, _, _, _, scanline, cycle = m.groups()
            scanline = int(scanline)
            if previous_line is not None and previous_line < VBLANK_LINE <= scanline:
                frame += 1
            previous_line = scanline
            if pending_read is not None:
                port, group = pending_read
                if current is not None and reads[port] < 8:
                    bit = int(m.group(group), 16) & 1
                    current[2][port] |= bit << reads[port]
                    current[4][port].append(bit)
                    # a DMC DMA that lands on the read stretches the 4-cycle LDA abs: the controller is clocked once more
                    if load_direct and int(cycle) - load_cycle > 4 and current[5][port] is None and not strobe_high:
                        current[5][port] = reads[port]
                if not strobe_high:  # (while the strobe is high every read returns A again)
                    reads[port] += 1
                if current is not None:
                    current[3][port] = reads[port]
                pending_read = None
            addr = target(operand)
            if addr not in (0x4016, 0x4017):
                continue
            if mnemonic in STORES and addr == 0x4016:
                if int(m.group(STORES[mnemonic]), 16) & 1:
                    strobe_high = True
                elif strobe_high: # the latch
                    strobe_high = False
                    reads[:] = [0, 0]
                    current = [frame, int(cycle), [0, 0], [0, 0], [[], []], [None, None]]
                    strobes.append(current)
            elif mnemonic in LOADS:
                pending_read = (addr - 0x4016, LOADS[mnemonic])
                load_cycle = int(cycle)
                # 4 cycles without a DMA: plain absolute, or absolute indexed without a page crossing (LDA $4016,X [$4016])
                indexed = re.match(r'\s*\$([0-9A-F]{2})[0-9A-F]{2},[XY]\s*\[\$([0-9A-F]{2})[0-9A-F]{2}\]', operand)
                load_direct = (re.match(r'\s*\$[0-9A-F]{4}\s*=', operand) is not None or
                               (indexed is not None and indexed.group(1) == indexed.group(2)))
            elif mnemonic == "AND" and (int(m.group(4), 16) & 1):  # AND #$03 / AND $4016: bit 0 of the result is the button
                pending_read = (addr - 0x4016, 4)
            elif mnemonic not in STORES:
                skipped += 1
    result = []
    for f, c, bits, n, seen, dma in strobes:
        fixed = list(bits)
        for port in (0, 1):
            k = dma[port]
            if k is not None and len(seen[port]) >= 8:
                # the DMA's extra clock consumed bit k: the reads from k on return the following bits, the 8th the 1 a controller
                # shifts in after its data. The consumed bit is unknown (taken as not pressed).
                true = seen[port][:k] + [0] + seen[port][k:7]
                fixed[port] = sum(b << i for i, b in enumerate(true))
        result.append((f, c, tuple(fixed), tuple(n), tuple(k is not None for k in dma)))
    return result, skipped


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("-o", "--output", help="input script to write (default: stdout)")
    args = ap.parse_args()

    strobes, skipped = read_strobes(args.log)
    lines = []
    last = (0, 0)
    i = 0
    while i < len(strobes):
        j = i
        while j < len(strobes) and strobes[j][0] == strobes[i][0]:
            j += 1
        group = strobes[i:j]
        # each port by the strobes that read it in full: a game may strobe once for each port, or read only the first
        # bit of an extra strobe
        pick = []
        for port in (0, 1):
            full = [s for s in group if s[3][port] >= 8]
            clean = [s for s in full if not s[4][port]]  # (a DMA-clocked read lost a bit: use it only if every strobe had one)
            full = clean or full
            counts = Counter(s[2][port] for s in (full or group)).most_common()
            value = counts[0][0]
            if len(counts) > 1 and counts[1][1] == counts[0][1] and last[port] in (counts[0][0], counts[1][0]):
                value = last[port]  # a tie: keep the buttons
            pick.append(value)
        best = tuple(pick)
        if best != last:
            lines.append(f"{strobes[i][1] - 1} {names(best[0])}" + (f" {names(best[1])}" if best[1] else "") + "\n")
            last = best
        i = j

    out = open(args.output, "w") if args.output else sys.stdout
    out.write(f"# controller input recovered from {args.log} by regression/input_from_log.py\n")
    out.writelines(lines)
    if args.output:
        out.close()
    print(f"input_from_log.py: {len(strobes)} strobes, {len(lines)} changes" +
          (f", {skipped} reads skipped (not LDA/LDX/LDY/AND)" if skipped else ""), file=sys.stderr)


if __name__ == "__main__":
    main()
