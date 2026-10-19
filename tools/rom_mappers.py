#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""List the iNES mapper numbers used by the ROMs in a directory of .zip files.

Usage:
    tools/rom_mappers.py DIRECTORY [-r] [--counts]

Each .zip holds one ROM (a .nes file); only its 16-byte iNES header is read. Output (stdout): for every mapper number
that occurs, the number, the count and the games (the .nes name inside the zip, else the zip's name), sorted by mapper.
NES 2.0 headers give a 12-bit mapper number (shown as is, so it can be above 255) and a submapper ("78.3").
-r searches subdirectories; --counts prints only the number and the count of games.
Zips without a valid iNES header are listed at the end.
"""
import argparse
import os
import sys
import zipfile
from collections import defaultdict


def read_header(zf):
    """Return (name, header bytes) of the first .nes member of an open zip, or (None, None)."""
    for info in zf.infolist():
        if info.filename.lower().endswith('.nes'):
            with zf.open(info) as f:
                return os.path.basename(info.filename), f.read(16)
    return None, None


def mapper_of(h):
    """(mapper, submapper or None) from a 16-byte header, or None if it is not iNES."""
    if len(h) < 16 or h[:4] != b'NES\x1a':
        return None
    mapper = (h[6] >> 4) | (h[7] & 0xF0)
    sub = None
    if (h[7] & 0x0C) == 0x08:  # NES 2.0
        mapper |= (h[8] & 0x0F) << 8
        sub = h[8] >> 4
    return mapper, sub


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('directory')
    ap.add_argument('-r', action='store_true', help='search subdirectories too')
    ap.add_argument('--counts', action='store_true', help='only the number of games per mapper')
    args = ap.parse_args()

    paths = []
    for root, dirs, files in os.walk(args.directory):
        paths += [os.path.join(root, f) for f in files if f.lower().endswith('.zip')]
        if not args.r:
            break
    games = defaultdict(list)
    bad = []
    for p in sorted(paths):
        try:
            with zipfile.ZipFile(p) as zf:
                name, h = read_header(zf)
        except (zipfile.BadZipFile, OSError) as e:
            bad.append((os.path.basename(p), str(e)))
            continue
        m = mapper_of(h) if h else None
        if m is None:
            bad.append((os.path.basename(p), 'no .nes with an iNES header'))
            continue
        label = name or os.path.basename(p)
        games[m[0]].append(label + ('' if not m[1] else ' [submapper %d]' % m[1]))

    for number in sorted(games):
        names = sorted(games[number], key=str.lower)
        if args.counts:
            print('%3d  %d' % (number, len(names)))
            continue
        print('Mapper %d (%d game%s)' % (number, len(names), '' if len(names) == 1 else 's'))
        for n in names:
            print('    ' + n)
        print()
    print('%d ROMs, %d mappers' % (sum(len(v) for v in games.values()), len(games)))
    if bad:
        print('\nSkipped:')
        for n, why in bad:
            print('    %s: %s' % (n, why))


if __name__ == '__main__':
    sys.exit(main())
