#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""The Zapper against Mesen: ONE runs tests/zapper/zapper_free.nes aimed as Mesen saw it
(zapper_input_mesen.txt, made by tools/mesen/zapper_record.lua), and every read of $4017 must return what Mesen's did, at
the same CPU cycle. The reference is the list of Mesen's reads, "CYCLE VALUE" per line (tests/zapper/zapper_reads.txt),
taken out of Mesen's trace log.

  zapper_check.py ONE            compare
  zapper_check.py --make TRACE   write the list from Mesen's trace (zapper_free_mesen.txt) to stdout, up to CYCLE_LIMIT
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIR = os.path.join(ROOT, "tests", "zapper")
CYCLE_LIMIT = 5700000
READ = re.compile(r"A:([0-9A-F]{2}) .*Cycle:(\d+)")


def trace_reads(path):
    """[(cycle, value)] of every LDA $4017 of a trace: the value is A in the next line (the AND that follows)"""
    reads = []
    pending = None
    with open(path, errors="replace") as trace:
        for line in trace:
            if pending is not None:
                m = READ.search(line)
                if m:
                    reads.append((pending, int(m.group(1), 16)))
                pending = None
            if "LDA $4017" in line:
                m = READ.search(line)
                if m:
                    cycle = int(m.group(2))
                    if cycle > CYCLE_LIMIT:
                        break
                    pending = cycle
    return reads


def main(argv):
    if len(argv) == 3 and argv[1] == "--make":
        for cycle, value in trace_reads(argv[2]):
            print("%d %02X" % (cycle, value))
        return 0
    if len(argv) != 2:
        print(__doc__)
        return 2
    expected = []
    with open(os.path.join(DIR, "zapper_reads.txt")) as reference:
        for line in reference:
            cycle, value = line.split()
            expected.append((int(cycle), int(value, 16)))
    with tempfile.TemporaryDirectory() as scratch:
        trace = os.path.join(scratch, "one.trace")
        subprocess.run([argv[1], "-H", "-t", "--trace-file", trace, "--input", os.path.join(DIR, "zapper_input_mesen.txt"),
                        "-k", str(CYCLE_LIMIT + 100000), os.path.join(DIR, "zapper_free.nes")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        got = trace_reads(trace)
    bad = [i for i in range(min(len(got), len(expected))) if got[i] != expected[i]]
    print("zapper_check: %d reads of $4017, %d different%s" % (len(expected), len(bad),
          "" if len(got) == len(expected) else " (ONE made %d reads)" % len(got)))
    for i in bad[:5]:
        print("  read %d: Mesen %s, ONE %s" % (i, expected[i], got[i]))
    return 0 if not bad and len(got) == len(expected) and expected else 1


sys.exit(main(sys.argv))
