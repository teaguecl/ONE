#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""subset_check.py MESEN_LOG [OURS]: every line of a sampled trace (one --trace-sample) must be a line of the Mesen log.

Lines are matched by their CPU cycle number (which starts again at a reset) (the trace is missing most lines, so line numbers cannot be used). OURS is
a file or stdin; lines that are not trace lines (messages) are ignored. Prints how many lines matched and the first
mismatches; exit status 1 if there is any."""
import re, sys

CYCLE = re.compile(r"Cycle:(\d+)")
TRACE = re.compile(r"^[0-9A-F]{4}  ")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    ours = open(argv[2], errors="replace") if len(argv) > 2 else sys.stdin
    matched = bad = 0
    shown = 0
    with open(argv[1], errors="replace") as mesen:
        want, have = None, None  # the Mesen line at or after the cycle asked for
        last = 0                 # the cycle of our previous line; a smaller one means a reset (the count starts again)
        resetting = False
        for line in ours:
            line = line.rstrip("\r\n")
            if not TRACE.match(line):
                continue
            c = CYCLE.search(line)
            if not c:
                continue
            cycle = int(c.group(1))
            if cycle < last:
                resetting = True
            last = cycle
            while have is None or have < cycle or resetting:
                before = have
                want = mesen.readline()
                if not want:
                    break
                want = want.rstrip("\r\n")
                m = CYCLE.search(want)
                have = int(m.group(1)) if m else have
                if resetting and before is not None and have < before:
                    resetting = False
            if not want or have != cycle:
                bad += 1
                if shown < 5:
                    shown += 1
                    print("no Mesen line at cycle %d:\n  ours:  %s" % (cycle, line))
            elif want != line:
                bad += 1
                if shown < 5:
                    shown += 1
                    print("DIFFERENT at cycle %d:\n  ours:  %s\n  mesen: %s" % (cycle, line, want))
            else:
                matched += 1
    print("subset_check: %d lines match, %d differ" % (matched, bad))
    return 1 if bad or matched == 0 else 0


sys.exit(main(sys.argv))
