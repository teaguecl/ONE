#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# The full test matrix: every step runs even when an earlier one fails, and a summary at the
# end says which failed.
#
#   bash tools/full_tests.sh [options]
#     --quick-regtest   the quick Mesen regression instead of the full one
#     --no-regtest      skip the Mesen regression
#
# Steps:
#   linux-debugger            configure, build, every ctest (ImGui debugger + menu)
#   linux-debug               configure, build, every ctest (AddressSanitizer, warnings as errors)
#   separation_check          gui/ uses the emulator through frontend.h only
#   regtest                   Mesen trace regression with build/linux-debugger/one (any UNEXPECTED difference fails)
#
# Each step's output goes to build/full_tests/<step>.log; only the summary is printed. Exit code 1 if a step failed.
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
regtest_mode=full
regtest=1
for arg in "$@"; do
    case $arg in
    --quick-regtest) regtest_mode=quick ;;
    --no-regtest) regtest=0 ;;
    -h | --help)
        sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "full_tests: unknown option $arg (--help lists them)"
        exit 2
        ;;
    esac
done

logs=build/full_tests
mkdir -p "$logs"
jobs=$(nproc)
failed=()
start_all=$SECONDS

step() { # step NAME COMMAND...: run it into build/full_tests/NAME.log, report ok / FAILED
    local name=$1
    shift
    local log="$logs/$name.log" start=$SECONDS
    if "$@" > "$log" 2>&1; then
        echo "full_tests: $name ok ($((SECONDS - start)) s)"
    else
        echo "full_tests: $name FAILED ($((SECONDS - start)) s) -- $log"
        failed+=("$name")
        return 1
    fi
}

tree() { # tree NAME DIR CONFIGURE...: configure, build and test one tree (no build or tests if a step before failed)
    local name=$1 dir=$2
    shift 2
    step "$name-configure" "$@" &&
        step "$name-build" cmake --build "$dir" -j "$jobs" &&
        step "$name-ctest" ctest --test-dir "$dir" -j "$jobs" --output-on-failure
}

regression() { # the trace regression: its exit code is 0 even when traces differ, so look for UNEXPECTED
    ONE=build/linux-debugger/one regression/regtest.sh "$regtest_mode" || return 1
}

tree linux-debugger build/linux-debugger cmake --preset linux-debugger
tree linux-debug build/linux-debug cmake --preset linux-debug
step separation_check python3 tools/separation_check.py
if [ $regtest = 1 ]; then
    if [ -x build/linux-debugger/one ] && [[ ! " ${failed[*]} " =~ " linux-debugger-build " ]]; then
        log="$logs/regtest-$regtest_mode.log"
        start=$SECONDS
        regression > "$log" 2>&1
        summary=$(grep "^identical:" "$log")
        if grep -q "UNEXPECTED" "$log" || [ -z "$summary" ]; then
            echo "full_tests: regtest-$regtest_mode FAILED ($((SECONDS - start)) s) -- $log"
            grep "UNEXPECTED" "$log" | head -n 20
            failed+=("regtest-$regtest_mode")
        else
            echo "full_tests: regtest-$regtest_mode ok ($((SECONDS - start)) s): $summary"
        fi
    else
        echo "full_tests: regtest skipped (build/linux-debugger/one was not built)"
        failed+=("regtest-$regtest_mode")
    fi
fi

echo "full_tests: $((SECONDS - start_all)) s in all"
if [ ${#failed[@]} -gt 0 ]; then
    echo "full_tests: FAILED: ${failed[*]}"
    exit 1
fi
echo "full_tests: OK"
