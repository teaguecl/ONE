#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Check one-debugger's background view (wbg save; ctest debugger_wbg) of nestest's menu: a valid 512x480 PNG; the
part the game window shows (scroll 0: the top-left nametable, 256x240) outlined in white at its corners and not
past them; the copy of it at the right (vertical mirroring) at half brightness, pixel for pixel.

    tests/bg_check.py FILE.png
"""
import struct
import sys
import zlib


def read_png(path, size=(512, 480)):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG file")
    pos, chunks = 8, {}
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        (crc,) = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])
        if zlib.crc32(kind + body) & 0xFFFFFFFF != crc:
            raise ValueError(f"bad CRC in chunk {kind!r}")
        chunks.setdefault(kind, b"")
        chunks[kind] += body
        pos += 12 + length
    width, height, depth, color = struct.unpack(">IIBB", chunks[b"IHDR"][:10])
    if (depth, color) != (8, 2) or (size is not None and (width, height) != size):
        raise ValueError(f"expected {size or 'any size'} 8-bit RGB, got {width}x{height} depth {depth} color {color}")
    raw = zlib.decompress(chunks[b"IDAT"])
    row = 1 + width * 3
    pixels = []
    for y in range(height):
        line = raw[y * row + 1:(y + 1) * row]
        pixels.append([(line[x * 3] << 16) | (line[x * 3 + 1] << 8) | line[x * 3 + 2] for x in range(width)])
    return pixels


def main():
    path = sys.argv[1]
    try:
        p = read_png(path)
    except (OSError, ValueError, KeyError, zlib.error) as error:
        return fail(path, str(error))
    white = 0xFFFFFF
    for x, y in [(0, 0), (255, 0), (0, 239), (255, 239)]:
        if p[y][x] != white:
            return fail(path, f"no outline at the corner ({x}, {y}): {p[y][x]:06X}")
    for x, y in [(256, 0), (0, 240), (256, 240)]:
        if p[y][x] == white:
            return fail(path, f"the outline goes past the screen at ({x}, {y})")
    lit = 0
    for y in range(8, 232):
        for x in range(8, 248):
            if p[y][x] != white and p[y][x] != 0:
                lit += 1
                if p[y][x + 256] != (p[y][x] >> 1) & 0x7F7F7F:
                    return fail(path, f"({x + 256}, {y}) is {p[y][x + 256]:06X}, not ({x}, {y}) {p[y][x]:06X} dimmed")
    if lit < 100:
        return fail(path, f"only {lit} drawn pixels: nestest's menu is not there")
    print(f"{path}: the view outlined at scroll 0, its mirror dimmed ({lit} pixels compared)")
    return 0


def fail(path, message):
    print(f"{path}: {message}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
