#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Emulation speed of a 'one' binary on the profiling targets: headless, unthrottled, best and mean of RUNS runs.
#
#   tools/perf/bench.sh [ONE_BINARY] [RUNS]      defaults: build/linux-debugger/one, 5
#
# Targets (each skipped when its ROM is missing; commercial_games/ is not in the repository):
#   smb3      Super Mario Bros. 3, frames 920-1340 of level 1-1 (tools/perf/smb3_input.txt plays it; the run starts
#             from a save state the script makes at frame 920, so only gameplay is timed)
#   karnov    Karnov (MMC3), 600 frames, START held from CPU cycle 6000000
#   smb       Super Mario Bros. (NROM), 600 frames, the same
# These are the ROMs and frames that were profiled. To see where the time goes, run a
# target under perf (kernel.perf_event_paranoid <= 2): perf record -e cpu-clock:u ONE ...; perf report --no-children
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
one=$(realpath "${1:-$root/build/linux-debugger/one}")
runs=${2:-5}
frame=29781 # CPU cycles per frame
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# time_runs NAME ARGS...: prints "NAME  best  mean" over $runs runs
time_runs() {
    local name=$1 t times=()
    shift
    for ((i = 0; i < runs; i++)); do
        /usr/bin/time -o "$work/time" -f "%e" "$one" -H -u "$@" >/dev/null 2>&1
        t=$(tail -1 "$work/time")
        times+=("$t")
    done
    printf '%s\n' "${times[@]}" | awk -v n="$name" '{s += $1; if (NR == 1 || $1 < b) b = $1}
        END {printf "%-8s best %.2f s   mean %.3f s   (%d runs)\n", n, b, s / NR, NR}'
}

echo "bench.sh: $one"
smb3="$root/commercial_games/Super Mario Bros. 3 (USA).nes"
if [ -f "$smb3" ]; then
    cp "$smb3" "$work/smb3.nes"
    "$one" -H -u --input "$root/tools/perf/smb3_input.txt" --state-save "$work/smb3.state" --state-frame 920 \
        -k $((925 * frame)) "$work/smb3.nes" >/dev/null 2>&1
    time_runs smb3 --input "$root/tools/perf/smb3_input.txt" --state-load "$work/smb3.state" -k $((1340 * frame)) \
        "$work/smb3.nes"
fi
for target in "karnov:Karnov (USA).nes" "smb:Super Mario Bros. (World).nes"; do
    rom="$root/commercial_games/${target#*:}"
    if [ -f "$rom" ]; then
        cp "$rom" "$work/${target%%:*}.nes"
        time_runs "${target%%:*}" -k $((600 * frame + 30)) -S 6000000 "$work/${target%%:*}.nes"
    fi
done
