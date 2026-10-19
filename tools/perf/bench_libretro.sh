#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Emulation speed of a libretro core, the way a frontend runs it: tests/libretro_host loads CORE and runs each ROM for
# FRAMES frames, unthrottled; best and median wall time of RUNS runs per ROM, and frames per second over all ROMs.
# Works on Linux and in MSYS2 (MinGW GCC); tools/perf/bench_libretro.ps1 is the PowerShell version (MSVC).
#
#   tools/perf/bench_libretro.sh [-n RUNS] [-f FRAMES] [-l LABEL] CORE HOST ROM...
#     CORE   one_libretro.so / one_libretro.dll       HOST   libretro_host (libretro_host.exe)
#     defaults: 5 runs, 3600 frames (one emulated minute) per ROM
#     LABEL  a name for the build, printed in the result lines ("msys2-pgo")
#
# The last line is machine readable: "RESULT label fps_median fps_best" (frames per second summed over the ROMs).
# For comparable numbers: the same ROMs and options for every build, a quiet machine (no other builds running),
# the power plan on "high performance", and one warm-up run (the script does it).
set -euo pipefail

runs=5 frames=3600 label=""
while getopts n:f:l: opt; do
    case $opt in
    n) runs=$OPTARG ;;
    f) frames=$OPTARG ;;
    l) label=$OPTARG ;;
    *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -ge 3 ] || { echo "usage: bench_libretro.sh [-n RUNS] [-f FRAMES] [-l LABEL] CORE HOST ROM..." >&2; exit 2; }
core=$1 host=$2
shift 2
[ -n "$label" ] || label=$(basename "$core")

median() { sort -n | awk '{a[NR] = $1} END {print (NR % 2) ? a[(NR + 1) / 2] : (a[NR / 2] + a[NR / 2 + 1]) / 2}'; }

echo "bench_libretro.sh: $label  core $core  $frames frames, best of $runs"
sum_med=0 sum_best=0 total_frames=0
for rom in "$@"; do
    "$host" "$core" "$rom" --train 300 >/dev/null 2>&1 || { echo "  $(basename "$rom"): did not run; skipped" >&2; continue; }
    times=()
    for ((i = 0; i < runs; i++)); do
        start=$EPOCHREALTIME
        "$host" "$core" "$rom" --train "$frames" >/dev/null 2>&1
        end=$EPOCHREALTIME
        times+=("$(awk -v a="$start" -v b="$end" 'BEGIN {printf "%.4f", b - a}')")
    done
    best=$(printf '%s\n' "${times[@]}" | sort -n | head -1)
    med=$(printf '%s\n' "${times[@]}" | median)
    printf '  %-44s best %7.3f s   median %7.3f s   %6.0f fps\n' "$(basename "$rom")" "$best" "$med" \
        "$(awk -v f="$frames" -v t="$med" 'BEGIN {print f / t}')"
    sum_med=$(awk -v a="$sum_med" -v b="$med" 'BEGIN {print a + b}')
    sum_best=$(awk -v a="$sum_best" -v b="$best" 'BEGIN {print a + b}')
    total_frames=$((total_frames + frames))
done
[ "$total_frames" -gt 0 ] || { echo "bench_libretro.sh: no ROM ran" >&2; exit 1; }
awk -v l="$label" -v f="$total_frames" -v m="$sum_med" -v b="$sum_best" \
    'BEGIN {printf "RESULT %s %.0f %.0f\n", l, f / m, f / b}'
