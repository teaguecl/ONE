#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""MMC5 expansion audio against Mesen's recording of tests/exrom/mmc5audio.nes (ctest apu_mmc5audio).

    tests/mmc5_audio_check.py ONE ROM MESEN.wav [--seconds 27.3] [--cycles 52000000]

Runs ONE headless for the ROM's schedule with audio.filters = none (the mixer's level as it is), writing a WAV, and compares it
with Mesen's: ONE's level goes through a first-order high-pass of 5.5 ms and a scale of 0.618 -- Mesen's own output chain,
fitted on the 2A03's own pulse in the same recording (part 10 of the ROM) -- and the peak-to-peak level of every
25 ms block must agree within 8 % (or 150 of 32768; a block at a note's start or end, whose level depends on a few ms of
timing, between half the lowest and 1.5 times the highest of its neighbours' levels), from the first sound to --seconds (the end of the ROM's sound, 27.3 s). Exit status 2: no recording (skipped)."""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import wave

TAU = 0.0055    # Mesen's high-pass (s)
SCALE = 0.618   # Mesen's output scale against ONE's mixer units * 32767
BLOCK = 1200    # 25 ms at 48 kHz


def load(path):
    w = wave.open(path)
    n, ch = w.getnframes(), w.getnchannels()
    return list(struct.unpack("<%dh" % (n * ch), w.readframes(n))[0::ch])


def onset(x, threshold):
    return next((i for i, v in enumerate(x) if abs(v - x[0]) > threshold), 0)


def highpass(x, tau, dt=1 / 48000.0):
    a = tau / (tau + dt)
    out, y, prev = [], 0.0, x[0]
    for v in x:
        y = a * (y + v - prev)
        prev = v
        out.append(y)
    return out


def blocks(x):
    return [max(x[i:i + BLOCK]) - min(x[i:i + BLOCK]) for i in range(0, len(x) - BLOCK + 1, BLOCK)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("one")
    ap.add_argument("rom")
    ap.add_argument("mesen")
    ap.add_argument("--seconds", type=float, default=27.3)
    ap.add_argument("--cycles", type=int, default=52000000, help="CPU cycles to run (the ROM's length)")
    args = ap.parse_args()
    if not os.path.exists(args.mesen):
        print("no recording: %s (skipped)" % args.mesen)
        return 2
    with tempfile.TemporaryDirectory() as tmp:
        cfg, wav = os.path.join(tmp, "one.cfg"), os.path.join(tmp, "one.wav")
        with open(cfg, "w") as f:
            f.write("audio.filters = none\n")
        subprocess.run([args.one, "--headless", "--unthrottled", "--no-sav", "--config", cfg, "--wav", wav, "-k", str(args.cycles),
                        args.rom], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        ours = [float(v) for v in load(wav)]
    mesen = load(args.mesen)
    om, oo = onset(mesen, 200), onset(ours, 30)
    mine = [v * SCALE for v in highpass(ours[oo:], TAU)]
    pm, po = blocks(mesen[om:]), blocks(mine)
    n = min(len(pm), len(po), int(args.seconds * 48000 / BLOCK))
    def agree(i):
        a, b = pm[i], po[i]
        near = [pm[j] for j in (i - 1, i, i + 1) if 0 <= j < len(pm)]
        if max(near) - min(near) > 0.4 * max(near):  # a note's start or end is here: its level depends on a few ms of timing
            return min(near) * 0.5 - 150 <= b <= max(near) * 1.5 + 150
        return abs(a - b) <= max(150, 0.08 * max(a, b))

    bad = [(round(i * BLOCK / 48000, 2), pm[i], round(po[i])) for i in range(1, n)  # (block 0: the filters' start)
           if not agree(i)]
    print("onsets: Mesen %.3f s, ours %.3f s; %d blocks of 25 ms compared; %d differ" % (om / 48000, oo / 48000, n - 1, len(bad)))
    for t, a, b in bad[:20]:
        print("  %6.2f s  Mesen %6d  ours %6d" % (t, a, b))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
