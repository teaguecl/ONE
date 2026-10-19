#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# One-step setup of a fresh (cloud) Linux machine for working on ONE: install the packages that are missing, configure
# and build one tree (clang, SDL2 window, ImGui menu, debugger, warnings as errors), then run the smoke tests.
#
#   bash tools/cloud_setup.sh [BUILD_DIR]      # default /tmp/bk
#
# All output goes to BUILD_DIR.setup.log; only a short summary is printed (it ends "cloud_setup: OK" or says which
# step failed and where to look). Running it again only rebuilds what changed.
#
# Environment: ONE_SETUP_TESTS=0 skips the tests; ONE_SETUP_TESTS=all runs every test, not just the smoke tests.
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build=${1:-/tmp/bk}
log="$build.setup.log"
tests=${ONE_SETUP_TESTS:-smoke}
: > "$log"

step() { # step NAME COMMAND...: run it into the log; on failure show the end of the log and stop
    local name=$1
    shift
    local start=$SECONDS
    if "$@" >> "$log" 2>&1; then
        echo "cloud_setup: $name ok ($((SECONDS - start)) s)"
    else
        echo "cloud_setup: $name FAILED -- last lines of $log:"
        tail -n 25 "$log"
        exit 1
    fi
}

# Packages: compilers, CMake/Ninja, SDL2, clang-format/clang-tidy for tools/format.sh
# and tools/tidy.sh, tmux for trying the text UI. Only the missing ones are installed.
packages=(build-essential clang cmake ninja-build pkg-config libsdl2-dev clang-format clang-tidy tmux)
missing=()
for p in "${packages[@]}"; do
    dpkg -s "$p" > /dev/null 2>&1 || missing+=("$p")
done
if [ ${#missing[@]} -gt 0 ]; then
    sudo=""
    [ "$(id -u)" -ne 0 ] && sudo=sudo
    # (not fatal: some sandboxes cannot reach the package mirrors; SDL2 is then built from source, below)
    if bash -c "$sudo apt-get update -q && DEBIAN_FRONTEND=noninteractive $sudo apt-get install -y -q ${missing[*]}" \
        >> "$log" 2>&1; then
        echo "cloud_setup: installed ${missing[*]}"
    else
        echo "cloud_setup: could not install ${missing[*]} (apt failed; see $log) -- going on without them"
    fi
fi
fetch_sdl=OFF
pkg-config --exists sdl2 2> /dev/null || fetch_sdl=ON # no libsdl2-dev: CMake downloads and builds SDL2

# Configure once (CMake downloads Lua and Dear ImGui into $build/_deps), then build
if [ ! -f "$build/CMakeCache.txt" ]; then
    step configure cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DONE_BUILD_DEBUGGER=ON -DONE_BUILD_GUI=ON -DONE_WARNINGS_AS_ERRORS=ON -DONE_FETCH_SDL2=$fetch_sdl
fi
step build cmake --build "$build" -j "$(nproc)"

case "$tests" in
0) ;;
all) step "tests (all)" ctest --test-dir "$build" -j "$(nproc)" --output-on-failure ;;
*) step "tests (smoke)" ctest --test-dir "$build" -L smoke -j "$(nproc)" --output-on-failure ;;
esac
echo "cloud_setup: OK -- build in $build, log in $log"
