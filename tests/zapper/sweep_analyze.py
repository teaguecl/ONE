#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Which colors does the Zapper see light in? Reads a trace log of tests/zapper/sweep.nes (Mesen's, or ONE's with -t) and the
aim recorded with it (tools/mesen/zapper_record.lua, "CYCLE zapper X Y [fire]" lines) and prints, per NES color, how many
$4017 reads inside the light window found light and how many did not, then the brightness bracket they give.

  sweep_analyze.py TRACE AIM [--palette video.c]      analyze (colors' luma from ONE's palette in video.c)
  sweep_analyze.py --make-input FILE                  write an --input script that visits every square of every page
  sweep_analyze.py --selftest ONE                     run ONE on the sweep ROM with that script and analyze its own trace

Per frame the ROM writes its page to $4018 ("STA $4018", A = page) before the polls; the aim is the one in force at the
first read of the frame; a read counts when the beam has drawn the aim pixel and is less than 21 scanlines past its line
(the window zapper.c uses: seen only there, a dark read outside it says nothing about the color)."""
import bisect
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import make_sweep  # noqa: E402

FRAME = 262 * 341
FRAME_CYCLES = 29781
REGS = re.compile(r"A:([0-9A-F]{2}) .*V:(-?\d+)\s+H:(\d+)\s+Cycle:(\d+)")


def load_palette(video_c):
    text = open(video_c).read()
    body = text[text.index("rgb_palette[64 * 3]"):]
    body = body[body.index("{") + 1:body.index("}")]
    values = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    return [(values[i * 3], values[i * 3 + 1], values[i * 3 + 2]) for i in range(64)]


def luma(rgb):
    """The measure Mesen's light sense fits: the sum of the channels (the luma weights 299/587/114 do not separate its lit and
    dark colors: $0A and $09 are dark, $08 and $0B lit)"""
    return rgb[0] + rgb[1] + rgb[2]


def read_aim(path):
    aim = []
    for line in open(path):
        parts = line.split("#")[0].split()
        if len(parts) >= 3 and parts[1] == "zapper":
            if parts[2] == "off":
                aim.append((int(parts[0]), None, None))
            else:
                aim.append((int(parts[0]), int(parts[2]), int(parts[3])))
    return aim


def analyze(trace, aim_file):
    aim = read_aim(aim_file)
    cycles = [a[0] for a in aim]
    seen = {}  # color -> [lit, dark]
    page = None
    frame_aim = None
    pending = None
    first_read = False
    with open(trace, errors="replace") as log:
        for line in log:
            if pending is not None:
                m = REGS.search(line)
                if m:
                    x, y = frame_aim
                    pos = pending[1]
                    since = (pos - y * 341) % FRAME
                    if x + 1 <= since < 21 * 341:
                        sq = make_sweep.square_at(x, y)
                        if sq is not None and page is not None:
                            color = make_sweep.page_colors(page)[sq[0] * 3 + sq[1]]
                            seen.setdefault(color, [0, 0])[0 if (int(m.group(1), 16) & 8) == 0 else 1] += 1
                pending = None
            if "STA $4018" in line:
                m = REGS.search(line)
                if m:
                    page = int(m.group(1), 16)
                    first_read = True
            elif "LDA $4017" in line:
                m = REGS.search(line)
                if m:
                    cycle = int(m.group(4))
                    if first_read:
                        first_read = False
                        i = bisect.bisect_right(cycles, cycle) - 1
                        frame_aim = None
                        if i >= 0 and aim[i][1] is not None:
                            frame_aim = (aim[i][1], aim[i][2])
                    if frame_aim is not None:
                        pending = (cycle, int(m.group(2)) * 341 + int(m.group(3)) + 9)
    return seen


def report(seen, palette):
    rows = []
    for color in range(64):
        lit, dark = seen.get(color, [0, 0])
        rows.append((color, lit, dark, luma(palette[color])))
    print("color   sum  rgb      light dark")
    for color, lit, dark, lum in sorted(rows, key=lambda r: r[3]):
        rgb = palette[color]
        print(" $%02X   %4d  %02X%02X%02X %6d %5d  %s" % (color, lum, *rgb, lit, dark,
              "MIXED" if lit and dark else ("light" if lit else ("dark" if dark else "-- not seen"))))
    lit_lumas = [r[3] for r in rows if r[1] and not r[2]]
    dark_lumas = [r[3] for r in rows if r[2] and not r[1]]
    mixed = [r[0] for r in rows if r[1] and r[2]]
    print("%d colors seen, %d only light, %d only dark, %d mixed %s" % (
          sum(1 for r in rows if r[1] or r[2]), len(lit_lumas), len(dark_lumas), len(mixed),
          ["$%02X" % c for c in mixed]))
    lo = max(dark_lumas, default=-1)
    hi = min(lit_lumas, default=None)
    print("brightness bracket (ONE's palette, R + G + B): the highest dark color is %d, the lowest lit one %s" % (lo, hi))
    return lo, hi, mixed


def make_input(path):
    """Every square of every page, 4 frames each, A to the next page"""
    lines = ["# visits every square of every page of tests/zapper/sweep.nes (sweep_analyze.py --make-input)"]
    cycle = 150000  # (the ROM is running by then)
    for page in range(make_sweep.PAGES):
        for r, sy in enumerate(make_sweep.ROWS_Y):
            for c, sx in enumerate(make_sweep.COLS_X):
                lines.append("%d zapper %d %d" % (cycle, sx + 16, sy + 16))
                cycle += 4 * FRAME_CYCLES
        lines.append("%d a" % (cycle - 2 * FRAME_CYCLES))
        lines.append("%d -" % (cycle - FRAME_CYCLES))
    open(path, "w").write("\n".join(lines) + "\n")
    return cycle


def main(argv):
    if len(argv) == 3 and argv[1] == "--make-input":
        make_input(argv[2])
        return 0
    palette_file = os.path.join(ROOT, "src", "video.c")
    if "--palette" in argv:
        palette_file = argv[argv.index("--palette") + 1]
    if len(argv) == 3 and argv[1] == "--selftest":
        with tempfile.TemporaryDirectory() as scratch:
            script, trace = os.path.join(scratch, "aim.txt"), os.path.join(scratch, "one.trace")
            end = make_input(script)
            subprocess.run([argv[2], "-H", "-t", "--trace-file", trace, "--input", script, "-k", str(end),
                            os.path.join(HERE, "sweep.nes")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            seen = analyze(trace, script)
        lo, hi, mixed = report(seen, load_palette(palette_file))
        ok = len(seen) >= 60 and not mixed and hi is not None and lo < hi
        print("zapper_sweep: %s (%d colors seen)" % ("ok" if ok else "FAILED", len(seen)))
        return 0 if ok else 1
    if len(argv) >= 3 and not argv[1].startswith("--"):
        report(analyze(argv[1], argv[2]), load_palette(palette_file))
        return 0
    print(__doc__)
    return 2


sys.exit(main(sys.argv))
