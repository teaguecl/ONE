#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Profile-guided build of 'one' (GCC or Clang), with LTO: 7-10% faster emulation than a plain Release build
# (tools/perf/bench.sh), with the same results (the emulation is unchanged; only the compiler's choices differ).
#
#   tools/pgo.sh [--libretro] [--windows] [BUILD_DIR] [CMAKE_OPTION...]    default BUILD_DIR: build/pgo[-libretro][-windows]
#
# --libretro builds the libretro core instead of 'one': one_libretro.so, trained by running the ROMs through
# tests/libretro_host (--train), the way a frontend runs them (one frame at a time, sound and picture callbacks).
# --windows (from Linux) cross-compiles with MinGW-w64 and trains under Wine: one_libretro.dll / one.exe. Wine is slow
# and its timings are not Windows': the profile (which code is hot) is what matters, not how fast the training ran.
# In MSYS2 (MinGW GCC) the script runs as it is and makes .dll / .exe by itself. MSVC: tools/pgo.ps1.
#
# 1. configures BUILD_DIR as a Release build with ONE_PGO=generate and link-time optimization (LTO), and builds an
#    instrumented 'one' (LTO adds 5-7% on top of PGO, nothing on a plain Release build: tools/perf/bench.sh,
#    2026-10-08; CMAKE_OPTION -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF turns it off);
# 2. runs ROMs in it headless (with sound mixing to a WAV file), which records where the time goes;
# 3. reconfigures the same directory with ONE_PGO=use and rebuilds everything with that profile.
#
# The training ROMs: PGO_ROMS="a.nes b.nes ..." if set, else the ROMs in the tree: nestest, the mapper and APU test
# ROMs, and if present test_roms/AccuracyCoin/AccuracyCoin.nes and every *.nes in commercial_games/ (or ONE_GAMES_DIR;
# their attract modes are typical game code: scrolling, sprites, music). With PGO_REQUIRE=FILE (tools/pgo_games.txt: one
# file name per line) it trains on exactly the games listed there, and fails if one is not in the directory: a
# release is always trained on the same games. ROMs with mappers ONE does not support yet are skipped.
# PGO_SECONDS: emulated seconds per ROM (default 20). Names with spaces need PGO_ROMS left unset.
# Run it again after changing the emulator: an old profile still works, but fits the new code less well.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
libretro=0
windows=0
while [ "${1:-}" = --libretro ] || [ "${1:-}" = --windows ]; do
    [ "$1" = --libretro ] && libretro=1 || windows=1
    shift
done
build=${1:-$root/build/pgo$([ $libretro = 1 ] && echo -libretro)$([ $windows = 1 ] && echo -windows)}
[ $# -gt 0 ] && shift
lib=so exe="" runner=() toolchain=()
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) lib=dll exe=.exe ;; esac
if [ $windows = 1 ]; then
    lib=dll exe=.exe
    command -v wine >/dev/null || { echo "pgo.sh: --windows needs wine (tools/dev.sh has it)" >&2; exit 1; }
    runner=(wine)
    export WINEPREFIX=${WINEPREFIX:-${TMPDIR:-/tmp}/one-wine-$(id -u)} # (not $HOME: in the container it is not ours)
    mkdir -p "$WINEPREFIX"
    export WINEDEBUG=${WINEDEBUG:--all}
    toolchain=(-DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/mingw-w64-x86_64.cmake")
fi
# a path as the program under training sees it (Wine wants Windows paths for the arguments)
native() { if [ $windows = 1 ]; then winepath -w "$1"; else echo "$1"; fi; }
seconds=${PGO_SECONDS:-20}
cycles=$((seconds * 1789773)) # NTSC CPU clock

if [ -n "${PGO_ROMS:-}" ]; then
    read -r -a roms <<<"$PGO_ROMS"
else
    roms=()
    for rom in tests/data/nestest.nes tests/mmc1/mmc1.nes tests/cnrom/cnrom.nes test_roms/apu_test/apu_test.nes \
        test_roms/apu_mixer/square.nes test_roms/apu_mixer/dmc.nes test_roms/AccuracyCoin/AccuracyCoin.nes; do
        [ -f "$root/$rom" ] && roms+=("$root/$rom")
    done
    games=${ONE_GAMES_DIR:-$root/commercial_games}
    if [ -n "${PGO_REQUIRE:-}" ]; then
        # exactly the games in that list (one file name per line, # for comments); the build fails if one is missing
        missing=0
        while IFS= read -r name; do
            case "$name" in '' | '#'*) continue ;; esac
            if [ -f "$games/$name" ]; then roms+=("$games/$name"); else echo "pgo.sh: missing game: $games/$name" >&2; missing=1; fi
        done <"$PGO_REQUIRE"
        [ $missing = 0 ] || { echo "pgo.sh: put the games listed in $PGO_REQUIRE into $games (ONE_GAMES_DIR)" >&2; exit 1; }
    else
        for rom in "$games"/*.nes; do
            [ -f "$rom" ] && roms+=("$rom")
        done
    fi
fi
[ ${#roms[@]} -gt 0 ] || { echo "pgo.sh: no training ROMs (set PGO_ROMS)" >&2; exit 1; }

profile="$build/pgo-profile"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "pgo.sh: 1/3 instrumented build in $build"
if [ $libretro = 1 ]; then
    targets=(one_libretro libretro_host)
    options=(-DONE_BUILD_LIBRETRO=ON -DONE_ENABLE_SDL2=OFF)
    product=$build/one_libretro.$lib
else
    targets=(one)
    options=()
    product=$build/one$exe
fi
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -DONE_PGO=generate \
    -DONE_PGO_DIR="$profile" "${toolchain[@]}" "${options[@]}" "$@" >/dev/null
rm -rf "$profile"
cmake --build "$build" --target "${targets[@]}" --parallel

echo "pgo.sh: 2/3 training: ${#roms[@]} ROMs, $seconds emulated seconds each"
for rom in "${roms[@]}"; do
    # a copy: the ROM's battery save and save states stay where they are
    cp "$rom" "$work/train.nes"
    rm -f "$work/train.sav"
    start=()
    case "$(basename "$rom")" in
    AccuracyCoin.nes) start=(-S 2000000) ;; # START runs all its tests
    esac
    echo "  $(basename "$rom")"
    if [ $libretro = 1 ]; then
        # 60 frames a second; AccuracyCoin's start-up (-S) has no counterpart here: it runs its menu
        "${runner[@]}" "$build/tests/libretro_host$exe" "$(native "$build/one_libretro.$lib")" "$(native "$work/train.nes")" \
            --train $((seconds * 60)) >/dev/null 2>&1
    else
        "${runner[@]}" "$build/one$exe" -H "${start[@]}" -k "$cycles" --wav "$(native "$work/train.wav")" \
            "$(native "$work/train.nes")" >/dev/null 2>&1
    fi || echo "pgo.sh: $(basename "$rom") did not run (unsupported mapper?); skipped" >&2
done

compiler=$(sed -n 's/^CMAKE_C_COMPILER:[A-Z]*=//p' "$build/CMakeCache.txt")
if "$compiler" --version 2>/dev/null | grep -qi clang; then
    # Clang writes raw profiles; they are merged into the one file ONE_PGO=use reads
    major=$("$compiler" -dumpversion | cut -d. -f1)
    profdata=$(command -v "llvm-profdata-$major" || command -v llvm-profdata) ||
        { echo "pgo.sh: llvm-profdata not found (Clang profiles need it)" >&2; exit 1; }
    "$profdata" merge -o "$profile/default.profdata" "$profile"/*.profraw
fi

echo "pgo.sh: 3/3 optimised build"
cmake -S "$root" -B "$build" -DONE_PGO=use >/dev/null
cmake --build "$build" --target "${targets[@]}" --parallel
echo "pgo.sh: done: $product"
