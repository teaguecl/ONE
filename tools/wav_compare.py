#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Compare two WAV recordings of the same sound (ONE's --wav against Mesen's Sound Recorder), block by block.

    tools/wav_compare.py MESEN.wav ONE.wav [--block 0.1] [--start-threshold 200] [--tolerance 0.05]

Both files are read as 16-bit PCM (the left channel when stereo; ONE writes mono, Mesen's recorder stereo), aligned on the
first sample louder than the threshold, and compared per block of 0.1 s: the peak-to-peak level and the number of zero
crossings (the pitch). Prints the blocks that differ by more than the tolerance (relative), a count, and the worst ones.
Exit status 0 when every block agrees."""
import argparse
import struct
import sys
import wave


def load(path):
    w = wave.open(path)
    n, ch, rate = w.getnframes(), w.getnchannels(), w.getframerate()
    data = struct.unpack("<%dh" % (n * ch), w.readframes(n))
    return list(data[0::ch]), rate


def onset(samples, threshold):
    for i, v in enumerate(samples):
        if abs(v) > threshold:
            return i
    return len(samples)


def blocks(samples, start, rate, size):
    step = int(rate * size)
    out = []
    for i in range(start, len(samples) - step + 1, step):
        seg = samples[i:i + step]
        crossings = sum(1 for a, b in zip(seg, seg[1:]) if (a < 0) != (b < 0))
        out.append((max(seg) - min(seg), crossings))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("ours")
    ap.add_argument("--block", type=float, default=0.1)
    ap.add_argument("--start-threshold", type=int, default=200)
    ap.add_argument("--tolerance", type=float, default=0.05)
    ap.add_argument("--quiet", action="store_true", help="only the summary")
    args = ap.parse_args()
    a, ra = load(args.reference)
    b, rb = load(args.ours)
    if ra != rb:
        print("sample rates differ: %d and %d" % (ra, rb))
        return 2
    sa, sb = onset(a, args.start_threshold), onset(b, args.start_threshold)
    ba, bb = blocks(a, sa, ra, args.block), blocks(b, sb, rb, args.block)
    n = min(len(ba), len(bb))
    bad = []
    for i in range(n):
        (pa, ca), (pb, cb) = ba[i], bb[i]
        level = abs(pa - pb) > args.tolerance * max(pa, pb, 1) and abs(pa - pb) > 40
        pitch = abs(ca - cb) > args.tolerance * max(ca, cb, 1) and abs(ca - cb) > 3
        if level or pitch:
            bad.append((i * args.block, pa, pb, ca, cb))
    print("onset: reference %.3f s, ours %.3f s; %d blocks of %.2f s compared (%d vs %d); %d differ" %
          (sa / ra, sb / rb, n, args.block, len(ba), len(bb), len(bad)))
    if not args.quiet:
        for t, pa, pb, ca, cb in bad[:60]:
            print("  %6.2f s  peak-to-peak %6d vs %6d   crossings %4d vs %4d" % (t, pa, pb, ca, cb))
    return 0 if not bad and len(ba) == len(bb) else 1


if __name__ == "__main__":
    sys.exit(main())
