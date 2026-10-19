#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Two PNGs (8-bit RGB, as ONE writes them; tests/bg_check.py reads them) are the same picture, pixel for pixel
(ctests debugger_lua_background and debugger_lua_palette: the example scripts against the wbg and wpal commands).

    tests/png_same.py A.png B.png
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bg_check import read_png  # noqa: E402


def main():
    try:
        a, b = read_png(sys.argv[1], None), read_png(sys.argv[2], None)
        if (len(a[0]), len(a)) != (len(b[0]), len(b)):
            raise ValueError(f"{len(a[0])}x{len(a)} and {len(b[0])}x{len(b)}")
    except (OSError, ValueError, KeyError) as error:
        print(f"png_same: {error}")
        return 1
    diff = [(x, y) for y in range(len(a)) for x in range(len(a[0])) if a[y][x] != b[y][x]]
    if diff:
        print(f"png_same: {len(diff)} pixels differ, the first at {diff[0]}")
        return 1
    print("png_same: the same picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
