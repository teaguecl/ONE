#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Check a screenshot written by ONE (ctest debugger_screenshot): a valid PNG (signature, chunk CRCs, zlib
stream), 256x240 RGB, and a real picture (more than one color).

    tests/png_check.py FILE.png
"""
import struct
import sys
import zlib


def main():
    data = open(sys.argv[1], "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return fail("not a PNG file")
    pos, chunks = 8, {}
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        (crc,) = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])
        if zlib.crc32(kind + body) & 0xFFFFFFFF != crc:
            return fail(f"bad CRC in chunk {kind!r}")
        chunks.setdefault(kind, b"")
        chunks[kind] += body
        pos += 12 + length
    width, height, depth, color = struct.unpack(">IIBB", chunks[b"IHDR"][:10])
    if (width, height, depth, color) != (256, 240, 8, 2):
        return fail(f"expected 256x240 8-bit RGB, got {width}x{height} depth {depth} color type {color}")
    raw = zlib.decompress(chunks[b"IDAT"])
    row = 1 + width * 3
    if len(raw) != row * height:
        return fail(f"image data is {len(raw)} bytes, expected {row * height}")
    colors = set()
    for y in range(height):
        line = raw[y * row:(y + 1) * row]
        if line[0] != 0:
            return fail(f"row {y}: filter {line[0]} (ONE writes 0)")
        colors.update(line[1 + x * 3:4 + x * 3] for x in range(width))
    if len(colors) < 2:
        return fail("the picture is a single color")
    print(f"{sys.argv[1]}: valid 256x240 PNG, {len(colors)} colors")
    return 0


def fail(message):
    print(f"{sys.argv[1]}: {message}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
