#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Rebuild the initial contents of a cartridge's battery RAM ($6000-$7FFF) from a Mesen trace log.

Usage:
    tools/mesen/ram_from_log.py LOG OUT.sav [--size 8192]

Mesen's trace shows the value an instruction's operand had before the instruction ran ("STA $7E9B = $FF", "STA $7E14,X [$7E1D] = $5B").
The first such value of each address is what the RAM held when the recording began -- the game's save file as Mesen loaded it.
The addresses the game never touched stay 0 (they cannot change anything the trace shows). Use it when a recording started
from a save (its .sav is overwritten when Mesen closes, so the copy on disk is the end state) and ONE has to start the same way.
"""
import argparse
import re
import sys

EFFECTIVE = re.compile(r'\[\$([67][0-9A-F]{3})\] = \$([0-9A-F]{2})')
DIRECT = re.compile(r'^\w{4}  [A-Z]{3} \$([67][0-9A-F]{3}) = \$([0-9A-F]{2})')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('out')
    ap.add_argument('--size', type=int, default=8192)
    args = ap.parse_args()
    ram = bytearray(args.size)
    seen = set()
    with open(args.log, errors='replace') as f:
        for line in f:
            if '= $' not in line:
                continue
            m = EFFECTIVE.search(line) or DIRECT.match(line)
            if not m:
                continue
            addr = int(m.group(1), 16)
            if addr in seen:
                continue
            seen.add(addr)
            ram[(addr - 0x6000) % args.size] = int(m.group(2), 16)
    with open(args.out, 'wb') as f:
        f.write(ram)
    print('%d addresses seen' % len(seen), file=sys.stderr)


if __name__ == '__main__':
    sys.exit(main())
