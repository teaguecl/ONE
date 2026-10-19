#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Turn a Mesen trace log written with register labels ("STA PpuControl_2000 = $00") back into the plain form
the regression compares against ("STA $2000 = $00"), keeping the fixed column layout.

Usage: tools/mesen/strip_labels.py LOG...   (rewrites each file in place)
"""
import os
import re
import sys

LABEL = re.compile(r'\b[A-Za-z][A-Za-z0-9_]*_([0-9A-F]{4})\b')
LINE = re.compile(r'^([0-9A-F]{4}  )(.*?)( +A:[0-9A-F]{2} .*)$')


def fix(line):
    if '_' not in line:
        return line
    m = LINE.match(line)
    if not m:
        return line
    disasm = LABEL.sub(r'$\1', m.group(2))
    # Mesen pads the instruction to 33 columns, or leaves one space when it is longer
    return m.group(1) + (disasm.ljust(33) if len(disasm) < 33 else disasm + ' ') + m.group(3).lstrip(' ')


for path in sys.argv[1:]:
    tmp = path + '.tmp'
    with open(path, encoding='latin-1', newline='') as src, open(tmp, 'w', encoding='latin-1', newline='') as dst:
        for line in src:
            end = '\r\n' if line.endswith('\r\n') else '\n' if line.endswith('\n') else ''
            dst.write(fix(line[:len(line) - len(end)]) + end)
    os.replace(tmp, path)
