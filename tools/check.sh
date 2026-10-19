#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# One entry point for the tests: builds what is needed, runs the right tests in parallel, prints one summary line
# and records the result against a hash of the source tree, so "is everything passing?" always has an answer.
#
#   bash tools/check.sh fast       every change (~1 min): both trees (ImGui debugger; ASan+UBSan, warnings as errors),
#                                  every test that is not labeled slow
#   bash tools/check.sh full       before hand-off: fast + the slow tests (Mesen quick regression, PPU catch-up, clang-tidy)
#                                  in the non-ASan tree
#   bash tools/check.sh nightly    full + ppu_catchup_all, the full Mesen regression, the quick
#                                  regression under ASan, a Clang build of the debugger tree (the others use the system
#                                  compiler: GCC), the libretro core under ASan, and the PGO + LTO build (tools/pgo.sh)
#                                  with the full Mesen regression
#   bash tools/check.sh status     what has passed against the current sources, and which files changed since
#
# Tiers include the ones above them: a passing full run at the current sources also covers fast.
# Which tree runs what: the ASan tree (build/linux-debug) runs only the tests labeled neither slow nor nightly: the
# long ones check emulation accuracy, which ASan does not help with, and it makes them ~3x slower.
# Labels (tests/CMakeLists.txt): slow = more than a few seconds; nightly = only in the nightly tier.
#
# Logs: build/check/<step>.log (only the summary is printed). Results: build/check/stamp.tsv, one line per run.
# Every step runs even when an earlier one fails. Exit code 1 if anything failed.
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root" || exit 2
out=build/check
stamp=$out/stamp.tsv
mkdir -p "$out"
jobs=$(nproc)
tier=${1:-fast}

# Hash of the sources that decide the tests' outcome (not build output, ROMs, docs, backup files). The manifest
# (path + hash per line) lets "status" name the files that changed since a run.
manifest() {
    {
        find . -maxdepth 1 -type f \( -name CMakeLists.txt -o -name CMakePresets.json \)
        find src debugger gui lua third_party libretro tests tools regression cmake -type f -not -path 'third_party/lua_scripts/*' \
            \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' -o -name '*.lua' -o -name '*.py' \
            -o -name '*.sh' -o -name '*.cmake' -o -name '*.txt' -o -name '*.json' -o -name '*.cfg' \) 2>/dev/null
    } | grep -v -e '~$' -e '/#' -e '__pycache__' | sed 's|^\./||' | sort | xargs sha256sum | awk '{print $2 "\t" $1}'
}
tree_hash() { manifest | sha256sum | cut -c1-12; }

if [ "$tier" = status ]; then
    now=$(tree_hash)
    manifest > "$out/now.manifest"
    echo "check: sources $now"
    for t in fast full nightly; do
        line=$(awk -F'\t' -v t=$t '$1 == t' "$stamp" 2>/dev/null | tail -n 1)
        if [ -z "$line" ]; then
            printf 'check: %-8s never run\n' "$t"
            continue
        fi
        IFS=$'\t' read -r _ result hash epoch secs detail <<< "$line"
        age=$(date -d "@$epoch" '+%F %H:%M')
        if [ "$hash" = "$now" ]; then
            state=current
        else
            state="STALE (sources changed since)"
        fi
        printf 'check: %-8s %-4s %s, %s s, %s  %s\n' "$t" "$result" "$age" "$secs" "$detail" "$state"
    done
    # Files changed since the newest run that has a manifest
    last=$(ls -t "$out"/*.manifest 2>/dev/null | grep -v now.manifest | head -n 1)
    if [ -n "$last" ]; then
        changed=$(diff <(cat "$last") "$out/now.manifest" | grep '^[<>]' | cut -f1 | sed 's/^[<>] //' | sort -u)
        if [ -n "$changed" ]; then
            echo "check: changed since $(basename "$last" .manifest): $(echo "$changed" | head -n 12 | tr '\n' ' ')"
        fi
    fi
    exit 0
fi

case $tier in
fast | full | nightly) ;;
-h | --help)
    sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
*)
    echo "check: unknown tier '$tier' (fast, full, nightly, status)"
    exit 2
    ;;
esac

failed=()
details=()
start_all=$SECONDS

step() { # step NAME COMMAND...: output to build/check/NAME.log; report ok / FAILED
    local name=$1 start=$SECONDS
    shift
    if "$@" > "$out/$name.log" 2>&1; then
        echo "check: $name ok ($((SECONDS - start)) s)"
    else
        echo "check: $name FAILED ($((SECONDS - start)) s) -- $out/$name.log"
        failed+=("$name")
        return 1
    fi
}

build() { # build NAME PRESET [CMAKE_ARGS...]
    local name=$1 preset=$2 dir=build/$1
    shift 2
    step "$name-configure" cmake --preset "$preset" -B "$dir" "$@" && step "$name-build" cmake --build "$dir" -j "$jobs"
}

tests() { # tests NAME DIR CTEST_ARGS...: run ctest, say how many passed and which failed
    local name=$1 dir=$2 start=$SECONDS log
    shift 2
    log=$out/$name.log
    ctest --test-dir "$dir" -j "$jobs" --output-on-failure "$@" > "$log" 2>&1
    local rc=$?
    local summary
    summary=$(grep -E 'tests passed' "$log" | tail -n 1 | sed 's/^ *//')
    local skipped
    skipped=$(grep -cE '\*\*\*Skipped|\.\.\.\s+Skipped' "$log")
    if [ $rc -eq 0 ]; then
        echo "check: $name ok ($((SECONDS - start)) s): ${summary:-no tests ran}${skipped:+ (skipped: $skipped)}"
        [ -n "$summary" ] || { failed+=("$name"); echo "check: $name FAILED: no tests ran"; }
    else
        echo "check: $name FAILED ($((SECONDS - start)) s): ${summary:-ctest error} -- $log"
        sed -n '/The following tests FAILED/,$p' "$log" | grep -E '^\s+[0-9]+ - ' | head -n 20 | sed 's/^ */check:   /'
        failed+=("$name")
    fi
    return $rc
}

regtest_full() { # regtest_full NAME ONE_BINARY: the trace regression (exit code 0 even when traces differ: look for UNEXPECTED)
    local name=$1 log=$out/$1.log start=$SECONDS
    ONE=$2 JOBS=$jobs bash regression/regtest.sh full > "$log" 2>&1
    if grep -q UNEXPECTED "$log" || ! grep -q '^identical:' "$log"; then
        echo "check: $name FAILED ($((SECONDS - start)) s) -- $log"
        grep UNEXPECTED "$log" | head -n 20 | sed 's/^ */check:   /'
        failed+=("$name")
    else
        echo "check: $name ok ($((SECONDS - start)) s): $(grep '^identical:' "$log")"
    fi
}

# fast: both trees, every test that is not slow
build linux-debugger linux-debugger && tests ctest-debugger-fast build/linux-debugger -LE 'slow|nightly'
build linux-debug linux-debug && tests ctest-asan-fast build/linux-debug -LE 'slow|nightly'
step separation_check python3 tools/separation_check.py

if [ "$tier" = full ] || [ "$tier" = nightly ]; then
    # the slow tests, in the non-ASan tree only
    [ -x build/linux-debugger/one ] && tests ctest-debugger-slow build/linux-debugger -L slow -LE nightly
fi

if [ "$tier" = nightly ]; then
    [ -x build/linux-debugger/one ] && tests ctest-debugger-nightly build/linux-debugger -L nightly
    [ -x build/linux-debugger/one ] && regtest_full regtest-full build/linux-debugger/one
    # the quick regression once under ASan: rare paths that only one ROM reaches
    [ -x build/linux-debug/one ] && tests ctest-asan-regression build/linux-debug -R '^regression_quick$'
    # the same debugger tree with Clang (the trees above use the system compiler, GCC)
    build linux-debugger-clang linux-debugger -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ &&
        tests ctest-clang build/linux-debugger-clang -LE 'slow|nightly'
    # the libretro core under ASan + UBSan, warnings as errors (CI builds it as Release only)
    build libretro-asan libretro -DCMAKE_BUILD_TYPE=Debug -DONE_ENABLE_SANITIZERS=ON -DONE_WARNINGS_AS_ERRORS=ON &&
        tests ctest-libretro-asan build/libretro-asan -L libretro
    # PGO + LTO ('one' only): the optimizer must not change a single trace, so the full regression runs on it
    step pgo bash tools/pgo.sh build/pgo && regtest_full regtest-pgo build/pgo/one
fi

secs=$((SECONDS - start_all))
if [ ${#failed[@]} -gt 0 ]; then
    result=FAIL
    detail="failed: ${failed[*]}"
else
    result=PASS
    detail=ok
fi
hash=$(tree_hash)
printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$tier" "$result" "$hash" "$(date +%s)" "$secs" "$detail" >> "$stamp"
manifest > "$out/$tier.manifest"
echo "check: $tier $([ $result = PASS ] && echo 'ALL PASS' || echo "FAILED: ${failed[*]}") ($secs s, sources $hash)"
[ $result = PASS ]
