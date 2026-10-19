#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Decode AccuracyCoin results from a ONE RAM dump.

Usage:
    ./one -H -S 2000000 -k 150000000 -D 0000-04FF test_roms/AccuracyCoin/AccuracyCoin.nes 2>/dev/null > ac_out.txt
    regression/accuracycoin.py ac_out.txt [--asm test_roms/AccuracyCoin/AccuracyCoin.asm] [--failures] [--compare mesen.txt]

The test list (page, name, result address) is read from AccuracyCoin.asm, so it stays in sync with the ROM.

Result byte (see TEST_Fail / AREROM_PageEvaluate in the source):
    $00          not run
    $FF          skipped
    bit 0 set    PASS; (value >> 2) != 0 means one of several accepted behaviours (drawn light blue)
    bit 1 set    FAIL, error code = value >> 2
Tests whose result lives in page $03 are "DRAW" tests (they only display information) and are not counted,
exactly like the ROM's own results table.

--compare FILE: FILE is an earlier output of this script, or a reference in the same "STATUS  page / name (error n)"
format (test_roms/AccuracyCoin/mesen_results.txt was transcribed from Mesen's results screen); prints only the tests whose
PASS/FAIL status or FAIL error code differs.
"""
import argparse
import os
import re
import sys


def parse_asm(path):
    """Return [(page_name, test_name, address)] in ROM order."""
    consts = {}
    lines = open(path, encoding='latin-1').read().splitlines()
    for line in lines:
        m = re.match(r'^\s*([A-Za-z_]\w*)\s*=\s*\$([0-9A-Fa-f]+)', line)
        if m:
            consts[m.group(1)] = int(m.group(2), 16)

    # page order from TableTable
    order = []
    in_table = False
    for line in lines:
        if line.startswith('TableTable:'):
            in_table = True
            continue
        if line.startswith('EndTableTable:'):
            break
        if in_table:
            m = re.match(r'^\s*\.word\s+(\w+)', line)
            if m:
                order.append(m.group(1))

    # each suite: label, .byte "Page name", then table "Test", $FF, result_addr, routine ... until .byte $FF
    suites = {}
    current = None
    for line in lines:
        m = re.match(r'^(Suite_\w+):', line)
        if m:
            current = m.group(1)
            suites[current] = {'name': None, 'tests': []}
            continue
        if current is None:
            continue
        if suites[current]['name'] is None:
            m = re.match(r'^\s*\.byte\s+"([^"]*)"', line)
            if m:
                suites[current]['name'] = m.group(1)
            continue
        m = re.match(r'^\s*table\s+"([^"]*)"\s*,\s*\$FF\s*,\s*(\w+)\s*,', line)
        if m:
            sym = m.group(2)
            if sym not in consts:
                sys.exit('accuracycoin.py: no address for %s in %s' % (sym, path))
            suites[current]['tests'].append((m.group(1), consts[sym]))
            continue
        if re.match(r'^\s*\.byte\s+\$FF', line):
            current = None

    tests = []
    for label in order:
        s = suites[label]
        for name, addr in s['tests']:
            tests.append((s['name'], name, addr))
    return tests


def parse_dump(path):
    mem = {}
    for line in open(path, errors='replace'):
        m = re.match(r'^RAM ([0-9A-F]{4}):(.*)$', line.rstrip('\n'))
        if not m:
            continue
        base = int(m.group(1), 16)
        fields = [m.group(2)[i:i + 3] for i in range(0, len(m.group(2)), 3)]
        for i, f in enumerate(fields):
            f = f.strip()
            if f:
                mem[base + i] = int(f, 16)
    return mem


def decode(value):
    if value is None:
        return 'NODUMP', ''
    if value == 0x00:
        return 'NOTRUN', ''
    if value == 0xFF:
        return 'SKIP', ''
    if value & 0x01:
        return 'PASS', ('(behaviour %d)' % (value >> 2)) if (value >> 2) else ''
    if value & 0x02:
        return 'FAIL', '(error %d)' % (value >> 2)
    return 'UNKNOWN', '($%02X)' % value


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument('dump', help='output of one ... -D 0000-04FF')
    ap.add_argument('--asm', default=os.path.join(here, '..', 'test_roms', 'AccuracyCoin', 'AccuracyCoin.asm'))
    ap.add_argument('--failures', action='store_true', help='only print tests that did not pass')
    ap.add_argument('--compare', help='earlier output of this script; print only differences')
    args = ap.parse_args()

    tests = parse_asm(args.asm)
    mem = parse_dump(args.dump)
    if not mem:
        sys.exit('accuracycoin.py: no "RAM xxxx:" lines in %s (did you pass -D 0000-04FF?)' % args.dump)

    running = mem.get(0x35)   # RunningAllTests: 1 while the run-all loop is active
    rows = []
    counts = {}
    for page, name, addr in tests:
        if (addr >> 8) == 0x03:
            continue          # DRAW test: informational only, not counted by the ROM either
        status, extra = decode(mem.get(addr))
        counts[status] = counts.get(status, 0) + 1
        rows.append((status, '%s / %s' % (page, name), extra))

    if args.compare:
        old = {}
        for line in open(args.compare):
            m = re.match(r'^(\w+)\s+(.+?)(\s+\(.*\))?\s*$', line.rstrip('\n'))
            if m and ' / ' in m.group(2):
                old[m.group(2)] = m.group(1) + ((' ' + m.group(3).strip()) if m.group(3) else '')
        # PASS behaviour numbers are ignored: only PASS/FAIL (and a FAIL's error code) are compared
        def key(s):
            return 'PASS' if s.startswith('PASS') else s
        ndiff = 0
        for status, test, extra in rows:
            new = status + ((' ' + extra) if extra else '')
            if test not in old:
                print('%-60s not in %s' % (test, args.compare))
                ndiff += 1
            elif key(old[test]) != key(new):
                print('%-60s %-18s ours: %s' % (test, old[test], new))
                ndiff += 1
        print('# %d difference(s) against %s' % (ndiff, args.compare))
        return

    for status, test, extra in rows:
        if args.failures and status == 'PASS':
            continue
        print('%-7s %s %s' % (status, test, extra))
    total = sum(counts.values())
    print('# %d tests: %s' % (total, ', '.join('%s %d' % (k, v) for k, v in sorted(counts.items()))))
    if running == 1 or counts.get('NOTRUN', 0) == total:
        print('# WARNING: the run did not finish (RunningAllTests=$%02X) -- raise -k or check -S'
              % (running if running is not None else 0))


if __name__ == '__main__':
    main()
