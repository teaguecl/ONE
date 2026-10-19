#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Compare two APU state logs, one line per frame: Mesen's (tools/mesen/apu_log.lua) and ONE's (one --apu-log).

    tools/apu_compare.py test_roms/apu_reference/apu_dmc.txt one_dmc.txt [--frames N] [--ignore REGEX] [--show K]

Lines are matched by frame number. Only keys present in both logs are compared (each emulator models some
internals the other does not). For every differing key it prints how many frames differ and the first few
frames with both values; the exit status is 0 if nothing differs, 1 otherwise.

By default the free-running phase of each channel is not compared (where a timer is in its count, the duty and
triangle step, the noise shift register, the DMC bit counter, and the output level those give at that instant):
Mesen's APU runs lazily, bringing a channel up to date only when something needs it, so the phase its Lua API
reports at the end of a frame lags by an amount ONE cannot reproduce. Everything else (periods, envelopes, sweeps,
length and linear counters, the DMC's address, length, buffer and output level, the frame counter) has to match.
--phase compares those too.

A difference in a single frame, with the frames before and after matching, is listed as a race and does not
fail the comparison (--strict: it does). Mesen's end-of-frame event and ONE's log point fall at the same CPU cycle
but not at the same moment inside it, so a register write or frame-counter clock in that very cycle is seen by
one log and not the other (in the test ROMs, about one frame in a thousand).
"""
import argparse
import re
import sys


PHASE = (r"\.timer\.timer$|\.timer\.lastOutput$|\.dutyPos$|\.sequencePosition$|^apu\.noise\.shiftRegister$"
         r"|^apu\.dmc\.bitsRemaining$")


def load(path):
    frames = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.startswith("frame="):
                continue
            fields = dict(item.split("=", 1) for item in line.split() if "=" in item)
            frames[int(fields["frame"])] = fields
    return frames


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference", help="Mesen log")
    ap.add_argument("log", help="ONE log")
    ap.add_argument("--frames", type=int, default=0, help="compare only the first N frames")
    ap.add_argument("--ignore", action="append", default=[], help="skip keys matching this regex (repeatable)")
    ap.add_argument("--show", type=int, default=3, help="example frames to print per key (default 3)")
    ap.add_argument("--phase", action="store_true", help="also compare the channels' free-running phase")
    ap.add_argument("--strict", action="store_true", help="single-frame differences fail too")
    args = ap.parse_args()

    ref, one = load(args.reference), load(args.log)
    frames = sorted(set(ref) & set(one))
    if args.frames:
        frames = [f for f in frames if f <= args.frames]
    if not frames:
        print("no frames in common")
        return 1
    ignore = [re.compile(r) for r in args.ignore]
    if not args.phase:
        ignore.append(re.compile(PHASE))
    keys = sorted(set(ref[frames[0]]) & set(one[frames[0]]) - {"frame"})
    keys = [k for k in keys if not any(r.search(k) for r in ignore)]
    only_ref = sorted(set(ref[frames[0]]) - set(one[frames[0]]))
    only_one = sorted(set(one[frames[0]]) - set(ref[frames[0]]))

    bad = {}
    for f in frames:
        r, o = ref[f], one[f]
        for k in keys:
            if r.get(k) != o.get(k):
                bad.setdefault(k, []).append(f)

    # a race: one frame differs, its neighbours (if any) match
    present = set(frames)
    races = {}
    if not args.strict:
        for k in list(bad):
            differs = set(bad[k])
            isolated = [f for f in bad[k] if (f - 1 not in differs) and (f + 1 not in differs)
                        and (f - 1 in present or f + 1 in present)]
            if isolated:
                races[k] = isolated
                bad[k] = [f for f in bad[k] if f not in isolated]
                if not bad[k]:
                    del bad[k]

    print(f"{len(frames)} frames ({frames[0]}..{frames[-1]}), {len(keys)} keys compared, "
          f"{len(only_ref)} only in the reference, {len(only_one)} only in the log")
    # earliest first: the first divergence usually explains the rest
    for k in sorted(bad, key=lambda k: bad[k][0]):
        examples = ", ".join(f"frame {f}: {ref[f][k]} vs {one[f][k]}" for f in bad[k][:args.show])
        print(f"  {k}: {len(bad[k])} frames differ; {examples}")
    for k in sorted(races, key=lambda k: races[k][0]):
        examples = ", ".join(f"frame {f}: {ref[f][k]} vs {one[f][k]}" for f in races[k][:args.show])
        print(f"  race {k}: {len(races[k])} single frames; {examples}")
    if not bad:
        print("all compared keys match" + (" (apart from races)" if races else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
