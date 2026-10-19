#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Mesen trace regression.
#   regression/regtest.sh quick          tests measured under 20 s (use most of the time)
#   regression/regtest.sh full           every test (after a big feature)
#   regression/regtest.sh <mode> <filter>   only tests whose name contains <filter> (e.g. "vbl/", "irq")
# Environment:
#   ONE=<path to emulator binary>  (default ./one; with CMake: build/one or build/Release/one.exe)     JOBS=<parallel runs>  (default: the CPU count, at most 8)
#   SAMPLE=<n>        with PPU catch-up on: a sampled trace (--trace-sample n; every n-th instruction on average), checked
#                     as a subset of the Mesen log (regression/subset_check.py). ONE_TRACE_SEED=<n> changes where.
#   BUDGET=<seconds>  stop starting new tests after this long (prints how to resume)
#   FROM=<n>          skip the first n selected tests (resume point)
#   MEASURE=1         write the measured times back into tests.txt
# Each test streams the trace straight into cmp, so no trace files are written.
cd "$(dirname "$0")/.." || exit 2
export ONE_HARDWARE_SPRITES=1   # the hardware's sprite limit, whatever one.cfg says (video.disable_sprite_limit)
MODE=${1:-quick}; FILTER=$2; ONE=${ONE:-./one}; JOBS=${JOBS:-$(n=$(nproc 2>/dev/null || echo 2); echo $((n > 8 ? 8 : n)))}; FROM=${FROM:-0}; QUICK_LIMIT=20
MAN=regression/tests.txt; RES=$(mktemp); START=$SECONDS
[ "$MODE" = quick ] || [ "$MODE" = full ] || { echo "usage: $0 quick|full [filter]"; exit 2; }

run_one() {   # name rom log args note
    local name=$1 rom=$2 log=$3 args=$4 note=$5 t0=$SECONDS cyc out line
    if [ ! -f "$rom" ] || [ ! -f "$log" ]; then          # ROMs / Mesen logs are not all in the repository
        printf '%-34s %-40s\n' "$name" "SKIPPED (rom or log missing)"
        echo "$name|0|SKIPPED" >> "$RES"
        return
    fi
    cyc=$(tail -1 "$log" | grep -oE 'Cycle:[0-9]+' | cut -d: -f2)
    if [ -n "$SAMPLE" ]; then
        out=$("$ONE" --headless -t --trace-sample "$SAMPLE" $args -k "$cyc" "$rom" 2>&1 >/dev/null | tr -d '\r' | python3 regression/subset_check.py "$log" | tail -1)
        case "$out" in *" 0 differ") out="" ;; *) out="line 0 $out" ;; esac
    else
        out=$("$ONE" --headless -t $args -k "$cyc" "$rom" 2>&1 >/dev/null | tr -d '\r' | grep -E '^[0-9A-F]{4}  ' | cmp - "$log" 2>&1)
    fi
    if [ -z "$out" ]; then line="IDENTICAL"
    else line="DIFFERS $(echo "$out" | grep -oE 'line [0-9]+|EOF.*' | head -1)${note:+  ($note)}"; fi
    printf '%-34s %-40s %4ds\n' "$name" "$line" $((SECONDS - t0))
    echo "$name|$((SECONDS - t0))|$line" >> "$RES"
}

mapfile -t TESTS < <(grep -v '^#' "$MAN" | awk -F'|' -v m="$MODE" -v f="$FILTER" -v q=$QUICK_LIMIT \
    '($1 < q || m == "full") && index($2, f)')
n=${#TESTS[@]}; i=$FROM; running=0
echo "$MODE regression: $((n - FROM)) tests"
while [ $i -lt $n ]; do
    if [ -n "$BUDGET" ] && [ $((SECONDS - START)) -ge "$BUDGET" ]; then
        wait; echo "BUDGET reached -- resume with: FROM=$i $0 $MODE $FILTER"; break; fi
    IFS='|' read -r secs name rom log args note <<< "${TESTS[$i]}"
    run_one "$name" "$rom" "$log" "$args" "$note" &
    running=$((running + 1)); i=$((i + 1))
    if [ $running -ge "$JOBS" ]; then wait -n; running=$((running - 1)); fi
done
wait
same=$(grep -c '|IDENTICAL' "$RES"); skipped=$(grep -c '|SKIPPED' "$RES")
diff=$(grep -v '|IDENTICAL' "$RES" | grep -vc '|SKIPPED')
echo "identical: $same   differ: $diff   skipped: $skipped   ($((SECONDS - START)) s)"
grep -v '|IDENTICAL' "$RES" | grep -v '|SKIPPED' | grep -v 'known:' | cut -d'|' -f1 | sed 's/^/  UNEXPECTED: /'
if [ -n "$MEASURE" ]; then
    while IFS='|' read -r name t rest; do
        awk -F'|' -v OFS='|' -v n="$name" -v t="$t" '!/^#/ && $2 == n { $1 = t } 1' "$MAN" > "$MAN.new" && mv "$MAN.new" "$MAN"
    done < <(grep -v '|SKIPPED' "$RES")
fi
rm -f "$RES"
