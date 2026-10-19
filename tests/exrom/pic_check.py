#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Checks the screenshots of the MMC5 picture tests (tests/exrom/gen.py: pic_*.nes, ctest debugger_mapper_exrom_pic).

    pic_check.py exattr pic_exattr.png
    pic_check.py split pic_split_left.png pic_split_right.png
    pic_check.py view pic_exattr_wbg.png pic_exattr_view.png            (the debugger's background window shows the picture)
    pic_check.py tables pic_split_8x16_wpat.png                          (the pattern window: two different tables)
    pic_check.py same pic_split_left.png pic_split_left_mesen.png   (every pixel like Mesen's screenshot, a channel 2 off allowed)

exattr: each tile's colour depends only on (bank & 3, palette) = (column & 3, row & 3) from its ExRAM byte (extended attributes),
and column & 3 == 0 is the backdrop in every row.  split: in pic_split_left.png the columns left of tile 16 show the split's bank
and palette, the others the main picture; pic_split_right.png the other way round, with the same two colours.
(exattr does not check the top row of tiles 0-1 of the first line: fetched before the frame starts.)"""
import struct
import sys
import zlib


def pixels(path):
    """A function (x, y) -> the pixel's 3 bytes of a PNG (any filter: Mesen's files use them)"""
    data = open(path, "rb").read()
    pos, chunks = 8, {}
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        chunks.setdefault(data[pos + 4:pos + 8], b"")
        chunks[data[pos + 4:pos + 8]] += data[pos + 8:pos + 8 + length]
        pos += 12 + length
    width, height, _, color = struct.unpack(">IIBB", chunks[b"IHDR"][:10])
    bpp = 3 if color == 2 else 4
    raw = zlib.decompress(chunks[b"IDAT"])
    stride = width * bpp
    rows, prev, p = [], bytearray(stride), 0
    for _ in range(height):
        kind, line, p = raw[p], bytearray(raw[p + 1:p + 1 + stride]), p + 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif kind == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
        rows.append(line)
        prev = line
    return lambda x, y: bytes(rows[y][x * bpp:x * bpp + 3])


def fail(msg):
    print("pic_check: " + msg)
    return 1


def exattr(path):
    px = pixels(path)
    seen = {}
    for r in range(1, 29):
        for c in range(32):
            key = (c & 3, r & 3) if (c & 3) else (0, 0)
            color = px(c * 8 + 4, r * 8 + 4)
            if seen.setdefault(key, color) != color:
                return fail("tile (%d, %d) is not like the other tiles of its bank / palette" % (c, r))
    if len(set(seen.values())) != len(seen):
        return fail("different banks / palettes drew the same colour")
    return 0


def split(left_path, right_path):
    colors = []
    for path, split_left in ((left_path, True), (right_path, False)):
        px = pixels(path)
        a = px(4, 100)    # left of the boundary
        b = px(252, 100)  # right of it
        if a == b:
            return fail("%s: the split changed nothing" % path)
        for y in range(8, 232):
            for x in range(256):
                want = a if x < 128 else b
                if px(x, y) != want:
                    return fail("%s: pixel (%d, %d) is not the colour of its side" % (path, x, y))
        colors.append((a, b) if split_left else (b, a))  # (split colour, main colour)
    if colors[0] != colors[1]:
        return fail("the split and the main picture are not drawn with the same colours in both pictures")
    return 0


def same(path, reference):
    """Every pixel like Mesen's screenshot. A palette colour may differ by 2 in a channel: some of Mesen's NES palette entries are
    1 off from ONE's ($26 ff8170 / fe8170, $1A 0d9300 / 0c9300), which is not the mapper's"""
    a, b = pixels(path), pixels(reference)
    for y in range(240):
        for x in range(256):
            if any(abs(p - q) > 2 for p, q in zip(a(x, y), b(x, y))):
                return fail("%s: pixel (%d, %d) differs from Mesen's %s" % (path, x, y, reference))
    return 0


def view(wbg_path, shot_path):
    """The background window's top left (scroll 0) is the picture: the pixels inside its outline, a channel 2 off allowed"""
    a, b = pixels(wbg_path), pixels(shot_path)
    for y in range(3, 237):
        for x in range(3, 253):
            if any(abs(p - q) > 2 for p, q in zip(a(x, y), b(x, y))):
                return fail("%s: pixel (%d, %d) is not the picture's %s" % (wbg_path, x, y, shot_path))
    return 0


def tables(path):
    """The pattern window of an 8x16 picture: the background's table (set B) and the other one (set A) show different CHR"""
    px = pixels(path)
    left, right = [px(4 + dx, 14 + dy) for dx in range(0, 8) for dy in range(0, 8)], \
                  [px(141 + dx, 14 + dy) for dx in range(0, 8) for dy in range(0, 8)]
    if left == right:
        return fail("%s: both tables show the same CHR" % path)
    return 0


def main():
    if sys.argv[1] == "exattr":
        return exattr(sys.argv[2])
    if sys.argv[1] == "same":
        return same(sys.argv[2], sys.argv[3])
    if sys.argv[1] == "view":
        return view(sys.argv[2], sys.argv[3])
    if sys.argv[1] == "tables":
        return tables(sys.argv[2])
    return split(sys.argv[2], sys.argv[3])


if __name__ == "__main__":
    sys.exit(main())
