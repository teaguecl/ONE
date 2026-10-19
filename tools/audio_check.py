#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Loudness profile of a WAV file, and the check for blargg's apu_mixer ROMs.

    tools/audio_check.py FILE.wav                       RMS of every 0.5 s (a quick look at a recording)
    tools/audio_check.py FILE.wav --quiet-between-beeps 0.05
                                                         exit 1 unless the stretch between the first and the
                                                         last beep is at most 5% as loud as the beeps
    tools/audio_check.py ONE.wav --reference MESEN.wav  both profiles, each relative to its own beep

The apu_mixer ROMs (square, triangle, dmc) beep, play a channel against the inverse wave made with the DMC, then
beep again: with correct mixing the middle is near silence. Loudness is relative to the beep, so recordings made
at different volumes (Mesen's, ONE's) can be compared. Stereo files are mixed to mono.
"""
import argparse
import math
import struct
import sys
import wave


def load(path):
    with wave.open(path) as w:
        if w.getsampwidth() != 2:
            sys.exit(f"{path}: only 16-bit WAV files are supported")
        rate, channels, n = w.getframerate(), w.getnchannels(), w.getnframes()
        data = struct.unpack(f"<{n * channels}h", w.readframes(n))
    if channels > 1:
        data = [sum(data[i:i + channels]) / channels for i in range(0, len(data), channels)]
    return rate, data


def profile(rate, data, window):
    step = max(1, int(rate * window))
    out = []
    for start in range(0, len(data) - step + 1, step):
        seg = data[start:start + step]
        mean = sum(seg) / len(seg)
        out.append(math.sqrt(sum((x - mean) ** 2 for x in seg) / len(seg)))
    return out


def beeps(levels):
    """(a beep's end, the next beep's start) as window indexes, or None: a beep is a run of windows at least
    half as loud as the loudest one."""
    loud = max(levels) if levels else 0
    if loud == 0:
        return None
    runs, start = [], None
    for i, level in enumerate(levels + [0]):
        if level >= loud / 2 and start is None:
            start = i
        elif level < loud / 2 and start is not None:
            runs.append((start, i))
            start = None
    if len(runs) < 2:
        return None
    # the test sound is the longest stretch between two beeps (a recording may begin with an earlier run's end)
    gaps = [(runs[i + 1][0] - runs[i][1], i) for i in range(len(runs) - 1)]
    _, i = max(gaps)
    return runs[i][1], runs[i + 1][0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav")
    ap.add_argument("--reference", help="a second recording (Mesen's) to show next to it")
    ap.add_argument("--quiet-between-beeps", type=float, metavar="RATIO",
                    help="fail if any 0.1 s between the beeps is louder than RATIO x the beep")
    args = ap.parse_args()

    if args.quiet_between_beeps is None:
        for path in [args.wav] + ([args.reference] if args.reference else []):
            rate, data = load(path)
            levels = profile(rate, data, 0.5)
            loud = max(levels) if levels else 1
            print(f"{path}: {len(data) / rate:.1f} s at {rate} Hz, loudest 0.5 s RMS {loud:.0f}")
            print("  % of loudest: " + " ".join(f"{100 * x / loud:.1f}" for x in levels))
        return 0

    rate, data = load(args.wav)
    window = 0.1
    levels = profile(rate, data, window)
    found = beeps(levels)
    if found is None:
        print(f"{args.wav}: did not find two beeps")
        return 1
    margin = int(0.5 / window)  # the filters' tail after a beep, and the note before the last one
    first, last = found[0] + margin, found[1] - margin
    beep = max(levels)
    quiet = levels[first:last]
    if not quiet:
        print(f"{args.wav}: the beeps are too close together")
        return 1
    worst = max(quiet)
    at = (first + quiet.index(worst)) * window
    mean = math.sqrt(sum(x * x for x in quiet) / len(quiet))
    ok = worst <= args.quiet_between_beeps * beep
    print(f"{args.wav}: between the beeps ({first * window:.1f}-{last * window:.1f} s) RMS {100 * mean / beep:.2f}% "
          f"of the beep, loudest {100 * worst / beep:.2f}% at {at:.1f} s (limit "
          f"{100 * args.quiet_between_beeps:.1f}%): {'ok' if ok else 'TOO LOUD'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
