#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Expansion audio of a played game against Mesen's recording of the same run (ctest apu_gimmick5b: the Sunsoft 5B, Gimmick!;
apu_akumajou: the VRC6, Akumajou Densetsu; apu_esperdream2: the VRC6b, Esper Dream 2: median 1.010, 17 of 974 blocks off,
rms 0.060; apu_rollingthunder: the Namco 163, 401 blocks: median 1.011, none off, rms 0.020; apu_finallap: the same chip, a short 9.6 s recording: median 1.008, none off,
rms 0.021; the first, 33 s recording drifted by seconds against its trace, probably samples dropped while Mesen logged).

    tests/gimmick_audio_check.py ONE ROM INPUT MESEN.wav [--cycles 39796437]   (Akumajou Densetsu: --cycles 77426804)

Runs ONE headless with the controller input recovered from Mesen's trace (regression/input_from_log.py) and audio.filters = none,
writing a WAV; ONE's level goes through Mesen's output chain (a first-order high-pass of 5.5 ms and a scale of 0.618, as in
tests/mmc5_audio_check.py) and the peak-to-peak level of every 25 ms block is compared with Mesen's, from the first sound on.
The 2A03 alone is 0.96 of Mesen's level with 177 of 786 blocks more than 20 % off (rms of the log ratio 0.69); with the 5B they
are 1.01, 15 and 0.07. Akumajou Densetsu (VRC6, 1570 blocks): median 1.000, none off, rms 0.016 (2A03 alone: 0.30). Fails over 40 blocks off by more than 20 %, a median outside 0.97-1.04 or an rms above 0.15.
Exit status 2: no recording or ROM (skipped)."""
import argparse
import math
import os
import statistics
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mmc5_audio_check as chain  # noqa: E402  (the same output chain and WAV helpers)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("one")
    ap.add_argument("rom")
    ap.add_argument("input")
    ap.add_argument("mesen")
    ap.add_argument("--cycles", type=int, default=39796437)
    ap.add_argument("--seconds", type=float, default=0.0, help="compare only the first SECONDS (a recording that drifts later)")
    ap.add_argument("--sav-in", help="battery RAM at the start of the recording (tools/mesen/ram_from_log.py)")
    args = ap.parse_args()
    for path in (args.rom, args.input, args.mesen):
        if not os.path.exists(path):
            print("missing %s (skipped)" % path)
            return 2
    with tempfile.TemporaryDirectory() as tmp:
        cfg, wav = os.path.join(tmp, "one.cfg"), os.path.join(tmp, "one.wav")
        with open(cfg, "w") as f:
            f.write("audio.filters = none\n")
        sav = ["--sav-in", args.sav_in] if args.sav_in else ["--no-sav"]
        subprocess.run([args.one, "--headless", "--unthrottled", *sav, "--config", cfg, "--input", args.input,
                        "--wav", wav, "-k", str(args.cycles), args.rom],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        ours = [float(v) for v in chain.load(wav)]
    mesen = chain.load(args.mesen)
    om, oo = chain.onset(mesen, 200), chain.onset(ours, 30)
    mine = [v * chain.SCALE for v in chain.highpass(ours[oo:], chain.TAU)]
    pm, po = chain.blocks(mesen[om:]), chain.blocks(mine)
    n = min(len(pm), len(po))
    if args.seconds:
        n = min(n, int(args.seconds * 48000 / chain.BLOCK))
    ratios = [max(po[i], 1) / pm[i] for i in range(1, n) if pm[i] > 1500]  # (block 0: the filters' start; quiet blocks: noise)
    median = statistics.median(ratios)
    off = sum(1 for r in ratios if abs(r - 1) > 0.2)
    rms = math.sqrt(sum(math.log(r) ** 2 for r in ratios) / len(ratios))
    print("onsets: Mesen %.3f s, ours %.3f s; %d blocks of 25 ms; median ratio %.3f, %d off by more than 20 %%, rms log error %.3f"
          % (om / 48000, oo / 48000, len(ratios), median, off, rms))
    return 0 if (0.97 <= median <= 1.04 and off <= 40 and rms <= 0.15) else 1


if __name__ == "__main__":
    sys.exit(main())
