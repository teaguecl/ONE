#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Turn a terminal screen capture (text with ANSI color codes, as written by capture.sh) into HTML.

    ansi2html.py CAPTURE.ansi [N@ROW,COL ...]

Each line becomes a <div>, each run of one color a <span>, so the PDF gets real (sharp, searchable) text instead of
a bitmap. N@ROW,COL adds a callout circle centred on that character cell (0-based) for the legend under the picture.
The stylesheet defines --line and --cell, the size of one character cell.
"""
import html
import re
import sys

# the 16 classic VGA text-mode colors (what the debugger's color names mean)
VGA = ['#000000', '#0000aa', '#00aa00', '#00aaaa', '#aa0000', '#aa00aa', '#aa5500', '#aaaaaa',
       '#555555', '#5555ff', '#55ff55', '#55ffff', '#ff5555', '#ff55ff', '#ffff55', '#ffffff']
# terminal order (ANSI 30-37 = black red green yellow blue magenta cyan white) -> VGA index
ANSI_TO_VGA = [0, 4, 2, 6, 1, 5, 3, 7]
DEFAULT_FG, DEFAULT_BG = VGA[7], VGA[0]


def xterm256(n):
    if n < 8:
        return VGA[ANSI_TO_VGA[n]]
    if n < 16:
        return VGA[ANSI_TO_VGA[n - 8] + 8]
    if n < 232:
        n -= 16
        level = lambda v: 0 if v == 0 else 55 + 40 * v
        return '#%02x%02x%02x' % (level(n // 36), level((n // 6) % 6), level(n % 6))
    g = 8 + 10 * (n - 232)
    return '#%02x%02x%02x' % (g, g, g)


def convert(text):
    fg, bg, bold = DEFAULT_FG, DEFAULT_BG, False
    lines = []
    for raw in text.rstrip('\n').split('\n'):
        spans = []
        for part in re.split(r'(\x1b\[[0-9;]*m)', raw):
            m = re.fullmatch(r'\x1b\[([0-9;]*)m', part)
            if m:
                codes = [int(c) if c else 0 for c in m.group(1).split(';')]
                i = 0
                while i < len(codes):
                    c = codes[i]
                    if c == 0:
                        fg, bg, bold = DEFAULT_FG, DEFAULT_BG, False
                    elif c == 1:
                        bold = True
                    elif c == 22:
                        bold = False
                    elif 30 <= c <= 37:
                        fg = VGA[ANSI_TO_VGA[c - 30]]
                    elif 90 <= c <= 97:
                        fg = VGA[ANSI_TO_VGA[c - 90] + 8]
                    elif 40 <= c <= 47:
                        bg = VGA[ANSI_TO_VGA[c - 40]]
                    elif 100 <= c <= 107:
                        bg = VGA[ANSI_TO_VGA[c - 100] + 8]
                    elif c == 39:
                        fg = DEFAULT_FG
                    elif c == 49:
                        bg = DEFAULT_BG
                    elif c in (38, 48) and i + 2 < len(codes) and codes[i + 1] == 5:
                        color = xterm256(codes[i + 2])
                        fg, bg = (color, bg) if c == 38 else (fg, color)
                        i += 2
                    elif c in (38, 48) and i + 4 < len(codes) and codes[i + 1] == 2:
                        color = '#%02x%02x%02x' % tuple(codes[i + 2:i + 5])
                        fg, bg = (color, bg) if c == 38 else (fg, color)
                        i += 4
                    i += 1
            elif part:
                style = 'color:%s;background:%s%s' % (fg, bg, ';font-weight:bold' if bold else '')
                spans.append('<span style="%s">%s</span>' % (style, html.escape(part)))
        lines.append('<div class="line">%s</div>' % ''.join(spans))
    return lines


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    with open(sys.argv[1], encoding='utf-8') as f:
        lines = convert(f.read())
    callouts = []
    for spec in sys.argv[2:]:
        m = re.fullmatch(r'(\w+)@(\d+),(\d+)', spec)
        if not m:
            sys.exit('bad callout %r (expected N@ROW,COL)' % spec)
        callouts.append('<span class="callout" style="top:calc(%s.5 * var(--line));left:calc(%s.5 * var(--cell))">%s</span>'
                        % (m.group(2), m.group(3), m.group(1)))
    print('<div class="screen">\n%s\n%s\n</div>' % ('\n'.join(lines), '\n'.join(callouts)))


if __name__ == '__main__':
    main()
