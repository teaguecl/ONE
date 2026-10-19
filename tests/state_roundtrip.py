#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Save-state round trip (ctest savestate_*): a state saved in the middle of a run, loaded into a fresh emulator,
must continue exactly like the original run.

    tests/state_roundtrip.py ONE ROM FRAME[,FRAME...] CYCLES [WORKDIR]

Run 1 plays ROM to CPU cycle CYCLES, saving a state at frame FRAME, with the CPU trace (-t) and the APU log
(--apu-log). Run 2 loads that state and plays to the same cycle. Every trace line of run 2 must be the
corresponding line of run 1 (CPU registers, cycle counts, PPU position for every instruction), and so must every
line of the APU log (all APU state, once per frame). A variable that the state leaves out makes them differ
sooner or later; the first difference is printed.

Then the whole machine is compared: run 2 also saves a state at frame FRAME + 20, and run 3 plays from power-on
to that frame and saves too. The two files must be identical byte for byte (all RAM, registers, the picture...).

With several frames (30,31,32,33) the whole round trip is done for each of them: a variable left out of the state
is only noticed if the program uses it before overwriting it, so saving at several points of the program's loop
catches more (tests/mmc1/mmc1.s needs four).
"""
import os
import subprocess
import sys


def run(one, args, workdir, name):
    trace_path = os.path.join(workdir, name + "_trace.txt")
    apu_path = os.path.join(workdir, name + "_apu.txt")
    with open(trace_path, "w") as err:
        proc = subprocess.run([one, "--headless", "--unthrottled", "-t", "--apu-log", apu_path] + args,
                              stdout=subprocess.PIPE, stderr=err, text=True)
    if proc.returncode != 0:
        sys.exit(f"{name}: one exited with status {proc.returncode}\n{proc.stdout[-2000:]}")
    with open(trace_path, errors="replace") as f:
        trace = [line for line in f.read().splitlines() if line]
    with open(apu_path) as f:
        apu = [line.split(" ", 1)[1] for line in f.read().splitlines() if line.startswith("frame=")]
    return trace, apu, proc.stdout


def roundtrip(one, rom, frame, cycles, workdir):
    state = os.path.join(workdir, os.path.basename(rom) + ".roundtrip.ss")
    if os.path.exists(state):
        os.remove(state)
    trace1, apu1, out1 = run(one, ["--state-save", state, "--state-frame", frame, "-k", cycles, rom], workdir,
                             "run1")
    if not os.path.exists(state):
        sys.exit(f"no state was saved:\n{out1[-2000:]}")
    later = str(int(frame) + 20)
    state2 = state + ".2"
    state3 = state + ".3"
    trace2, apu2, _ = run(one, ["--state-load", state, "--state-save", state2, "--state-frame", later, "-k", cycles,
                                rom], workdir, "run2")
    if not trace2:
        sys.exit("run 2 traced nothing")

    # run 2 starts at the instruction where the state was taken: find it in run 1 (by its cycle count)
    start = None
    for i, line in enumerate(trace1):
        if line == trace2[0]:
            start = i
            break
    if start is None:
        sys.exit(f"the first instruction of run 2 is not in run 1:\n  {trace2[0]}")
    tail = trace1[start:]
    if len(tail) != len(trace2):
        print(f"run 1 has {len(tail)} trace lines after the save, run 2 has {len(trace2)}")
    for i, (a, b) in enumerate(zip(tail, trace2)):
        if a != b:
            sys.exit(f"trace differs after {i} instructions:\n  run 1: {a}\n  run 2: {b}")
    if len(tail) != len(trace2):
        sys.exit("trace lengths differ")

    # the APU log: run 2's frames are the last ones of run 1
    if not apu2 or apu1[-len(apu2):] != apu2:
        for i, b in enumerate(apu2):
            a = apu1[len(apu1) - len(apu2) + i] if len(apu1) >= len(apu2) else "(missing)"
            if a != b:
                diff = [f"{x} / {y}" for x, y in zip(a.split(), b.split()) if x != y][:5]
                sys.exit(f"APU log differs in run 2's frame {i + 1}: " + ", ".join(diff))
        sys.exit("APU logs differ")
    run(one, ["--state-save", state3, "--state-frame", later, "-k", cycles, rom], workdir, "run3")
    with open(state2, "rb") as f2, open(state3, "rb") as f3:
        b2, b3 = f2.read(), f3.read()
    if b2 != b3:
        where = next((i for i in range(min(len(b2), len(b3))) if b2[i] != b3[i]), min(len(b2), len(b3)))
        sys.exit(f"the states saved at frame {later} differ (from byte {where} of {len(b2)})")
    print(f"{os.path.basename(rom)} frame {frame}: state saved at instruction {start + 1} of {len(trace1)}; run 2 matches run 1 "
          f"for {len(trace2)} instructions and {len(apu2)} frames of APU state; the states of frame {later} are identical")


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    one, rom, frames, cycles = sys.argv[1], sys.argv[2], sys.argv[3].split(","), sys.argv[4]
    # a directory of its own for each ROM, so that the tests can run in parallel (ctest -j)
    workdir = os.path.join(sys.argv[5] if len(sys.argv) > 5 else ".", "roundtrip_" + os.path.basename(rom))
    os.makedirs(workdir, exist_ok=True)
    for frame in frames:
        try:
            roundtrip(one, rom, frame, cycles, workdir)
        except SystemExit as e:
            if e.code in (None, 0):
                raise
            sys.exit(f"{os.path.basename(rom)}, state saved at frame {frame}: {e.code}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
