#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""The debugger's memory search (ctest debugger_search_stretch): filters typed one after another judge the same
stretch of play, so their order does not matter, and a stretch that ends before the game runs again is not empty.

    tests/search_check.py ONE-DEBUGGER ROM

Runs tests/mmc1/mmc1.nes (it writes RAM all the time) for 10 frames, then "cs same" + "cs notwritten", and again
from the start the other way round: both must leave the same number of candidates, fewer than "cs same" alone
(bytes the game rewrote with the same value are not "notwritten"). Then "cs undo" twice must give back the stretch.
"""
import re
import subprocess
import sys
import tempfile


def counts(debugger, rom, filters):
    run = ["cs start"] + [f"gsl {line}" for _ in range(10) for line in (100, 200)] + filters + ["q"]
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write("\n".join(run) + "\n")
        script = f.name
    out = subprocess.run([debugger, "--batch", "--log", script + ".log", "-s", script, rom], capture_output=True,
                         text=True, timeout=120).stdout
    return [int(n) for n in re.findall(r"(?:^|\n)(?:Back to )?(\d+) candidates", out)]


def main():
    debugger, rom = sys.argv[1], sys.argv[2]
    a = counts(debugger, rom, ["cs same", "cs notwritten", "cs undo", "cs undo", "cs notwritten"])
    b = counts(debugger, rom, ["cs notwritten", "cs same"])
    print("cs same, cs notwritten, cs undo, cs undo, cs notwritten:", a)
    print("cs notwritten, cs same:", b)
    ok = (len(a) == 5 and len(b) == 2 and a[1] == b[1] and a[1] < a[0] and a[3] == 10240 and a[4] == b[0])
    print("search_check.py: " + ("filters on one stretch agree" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
