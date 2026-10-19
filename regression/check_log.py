#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Check a Mesen trace log before it goes into the regression manifest (regression/tests.txt).

    regression/check_log.py ROM LOG [--trim OUT]

A log for regtest.sh must start at power-on (Mesen's first instruction is at cycle 8, at the ROM's reset vector) and
run long enough to test something. This reads the whole log once and reports:
  - lines that are not trace lines, and cycles that do not increase;
  - where it starts: a log that starts later (file logging turned on after the power cycle) cannot be used;
  - a power cycle inside the log (the cycle count starting again): --trim OUT writes the part from the last power-on;
  - how many frames it covers, and whether it ends at the end of a frame (scanline 239, dot 330+: where Mesen pauses
    -- e.g. when its window goes to the background -- which usually means the log was cut short).
Problems make it unusable; warnings (short, or ended at a pause) only make it weak.
Exit status: 0 = usable (maybe with warnings), 1 = not usable as it is, 2 = cannot read the files.
"""
import re
import sys

LINE = re.compile(rb"^([0-9A-F]{4})  .*?V:(-?\d+)\s+H:(\d+)\s+Cycle:(\d+)")  # (V:-1: the pre-render line)
FRAME = 29780.5  # NTSC CPU cycles per frame


def reset_vector(rom):
    data = open(rom, "rb").read()
    if data[:4] != b"NES\x1a":
        raise ValueError(f"{rom} is not an iNES file")
    prg = data[16 + (512 if data[6] & 4 else 0):][:16384 * data[4]]
    return prg[-4] | (prg[-3] << 8)


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    trim = argv[argv.index("--trim") + 1] if "--trim" in argv else None
    if trim:
        args.remove(trim)
    if len(args) != 2:
        print(__doc__)
        return 2
    rom, log = args
    try:
        reset = reset_vector(rom)
        lines = open(log, "rb")
    except (OSError, ValueError, IndexError) as error:
        print(f"check_log: {error}")
        return 2

    problems, warnings, notes = [], [], []
    first = last = None
    power_ons = []  # line numbers where a run starts (cycle <= 20)
    bad, previous, count = 0, -1, 0
    with lines:
        for number, line in enumerate(lines, 1):
            m = LINE.match(line)
            if not m:
                bad += 1
                if bad <= 3:
                    problems.append(f"line {number} is not a trace line: {line[:60]!r}")
                continue
            count += 1
            pc, v, h, cycle = int(m.group(1), 16), int(m.group(2)), int(m.group(3)), int(m.group(4))
            if first is None:
                first = (number, pc, cycle)
                if cycle <= 20:
                    power_ons.append(number)
            elif cycle < previous:
                if cycle <= 20:
                    power_ons.append(number)  # (the count starts again: a power cycle)
                else:
                    problems.append(f"line {number}: the cycle count goes back ({previous} -> {cycle})")
            previous = cycle
            last = (number, v, h, cycle)
    if first is None:
        print(f"{log}: no trace lines")
        return 1
    if bad > 3:
        problems.append(f"{bad} lines in all are not trace lines")

    start_line = power_ons[-1] if power_ons else None
    if start_line is None:
        problems.append(f"it does not start at power-on: the first line is at cycle {first[2]} (file logging was "
                        "probably turned on after the power cycle) -- record again: start logging, then power cycle")
    elif start_line != first[0] and trim:
        notes.append(f"a power cycle at line {start_line}: the lines before it are another run -- {trim} has the "
                     "part from there (check that one, and use it)")
    elif start_line != first[0]:
        problems.append(f"a power cycle at line {start_line}: the lines before it are another run -- --trim OUT "
                        "writes the part from there")
    if first[1] != reset and start_line == first[0]:
        notes.append(f"the first instruction is at ${first[1]:04X}, not the reset vector ${reset:04X} "
                     "(fine for nestest's automation mode, -p C000; otherwise a different ROM?)")

    _, v, h, end = last
    frames = (end - (first[2] if start_line is None else 8)) / FRAME
    if v == 239 and h >= 330:
        warnings.append(f"it ends at the end of a frame (scanline {v}, dot {h}): Mesen paused there, so it was probably "
                        "cut short -- keep Mesen's window in front (or turn off pausing in the background) while logging")
    if frames < 3:
        warnings.append(f"only {frames:.1f} frames long: it tests little (fine if the test ends that soon, as nestest's)")

    if trim and start_line is not None and start_line != first[0]:
        with open(log, "rb") as source, open(trim, "wb") as out:
            for number, line in enumerate(source, 1):
                if number >= start_line:
                    out.write(line)

    print(f"{log}: {count} lines, {frames:.1f} frames, ends at scanline {v} dot {h} (cycle {end})")
    for n in notes:
        print(f"  note: {n}")
    for w in warnings:
        print(f"  warning: {w}")
    for p in problems:
        print(f"  PROBLEM: {p}")
    print("  not usable as it is" if problems else "  usable, but see the warnings" if warnings else "  usable")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
