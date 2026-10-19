#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Re-make the screen captures used by the quick reference (images/screen-*.ansi): runs one-debugger in a tmux
# session, types a few commands and saves the screen with its colors (ANSI escape codes). Needs tmux and a built
# one-debugger. The captures are committed, so this only has to be run when the debugger's look changes.
#   capture.sh path/to/one-debugger path/to/nestest.nes
set -euo pipefail
DEBUGGER=${1:?usage: capture.sh ONE-DEBUGGER NESTEST.NES}
ROM=${2:?usage: capture.sh ONE-DEBUGGER NESTEST.NES}
HERE=$(cd "$(dirname "$0")" && pwd)
LABELS=$(mktemp)
printf 'al 00C72D .test_branches\nal 00C7DB .test_flags\nal 000000 .result\n' > "$LABELS"
for theme in borland softice; do
    tmux kill-session -t quickref 2>/dev/null || true
    tmux new-session -d -s quickref -x 100 -y 34 \
        "TERM=xterm-256color $DEBUGGER -H --theme $theme -l $LABELS --log /dev/null $ROM; sleep 30"
    sleep 1.5
    for cmd in "r pc = c000" "bpx test_branches" "bpx c735" "x" "t 2"; do
        tmux send-keys -t quickref "$cmd" Enter
        sleep 0.4
    done
    tmux capture-pane -t quickref -p -e > "$HERE/images/screen-$theme.ansi"
    tmux kill-session -t quickref
done
rm -f "$LABELS"
echo "capture.sh: wrote images/screen-borland.ansi and images/screen-softice.ansi"
