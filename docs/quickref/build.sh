#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Build the one-debugger quick reference PDF.
#   docs/quickref/build.sh [OUTPUT.pdf]          (default: docs/quickref/one-debugger-quickref.pdf)
# Needs pandoc (sudo apt install pandoc), python3, and Chrome or Chromium for printing the HTML to PDF
# (set CHROME=/path/to/chrome if it is not on the PATH).
#
#   quickref.md   the text (Pandoc Markdown); <!-- @include NAME --> and <!-- @screen FILE ... --> placeholders
#   extract.sh    pulls commands, keys, windows, options and settings out of the debugger source (grep/sed/awk)
#   ansi2html.py  turns the screen captures in images/ (made by capture.sh) into HTML
#   quickref.css  the layout
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
OUT=$(realpath -m "${1:-$HERE/one-debugger-quickref.pdf}")
WORK=$(mktemp -d)
[ -n "${KEEP:-}" ] && echo "build.sh: keeping $WORK" || trap 'rm -rf "$WORK"' EXIT

command -v pandoc >/dev/null || { echo "build.sh: pandoc not found (sudo apt install pandoc)" >&2; exit 1; }
if [ -z "${CHROME:-}" ]; then
    for c in google-chrome google-chrome-stable chromium chromium-browser; do
        if command -v "$c" >/dev/null; then CHROME=$c; break; fi
    done
fi
[ -n "${CHROME:-}" ] || { echo "build.sh: Chrome/Chromium not found (set CHROME=/path/to/chrome)" >&2; exit 1; }

# 1. facts from the source
bash "$HERE/extract.sh" "$SRC" "$WORK" # (through bash: works without the execute bit)
version=$(git -C "$SRC" describe --always --dirty 2>/dev/null || true)
echo "$(date +%Y-%m-%d)${version:+ (git $version)}" > "$WORK/version.html"

# 2. fill in the placeholders
awk -v work="$WORK" -v here="$HERE" '
    /^[ \t]*<!-- @include [a-z]+ -->[ \t]*$/ {
        name = $0; sub(/.*@include /, "", name); sub(/ -->.*/, "", name)
        file = work "/" name ".html"
        if ((getline line < file) <= 0) { print "build.sh: no fragment " file > "/dev/stderr"; exit 1 }
        print "```{=html}"                           # raw HTML: pandoc must not reformat it
        do print line; while ((getline line < file) > 0)
        print "```"
        close(file); next
    }
    /^[ \t]*<!-- @screen / {
        args = $0; sub(/.*@screen /, "", args); sub(/ -->.*/, "", args)
        file = args; callouts = ""
        if (match(args, / /)) { file = substr(args, 1, RSTART - 1); callouts = substr(args, RSTART) }
        cmd = "python3 \"" here "/ansi2html.py\" \"" here "/" file "\"" callouts
        if ((cmd | getline line) <= 0) { print "build.sh: cannot convert " args > "/dev/stderr"; exit 1 }
        print "```{=html}"
        do print line; while ((cmd | getline line) > 0)
        print "```"
        close(cmd); next
    }
    /<!-- @include .* -->/ {                          # inline placeholder (inside a line of text)
        while (match($0, /<!-- @include [a-z]+ -->/)) {
            name = substr($0, RSTART + 14, RLENGTH - 18)
            file = work "/" name ".html"; text = ""
            while ((getline line < file) > 0) text = text line
            close(file)
            $0 = substr($0, 1, RSTART - 1) text substr($0, RSTART + RLENGTH)
        }
    }
    { print }' "$HERE/quickref.md" > "$WORK/quickref.md"

# 3. Markdown -> HTML -> PDF
pandoc "$WORK/quickref.md" --from markdown --to html5 --standalone --css "file://$HERE/quickref.css" \
    --output "$WORK/quickref.html"
sandbox=()
# Chrome refuses to run as root with its sandbox, and the sandbox cannot start in a container (docker/Dockerfile
# sets CHROME_NO_SANDBOX=1)
if [ "$(id -u)" -eq 0 ] || [ -n "${CHROME_NO_SANDBOX:-}" ]; then
    sandbox=(--no-sandbox)
fi
# a profile of its own, so a Chrome that is already open is left alone
"$CHROME" --headless --disable-gpu "${sandbox[@]}" --user-data-dir="$WORK/chrome" \
    --no-pdf-header-footer --print-to-pdf-no-header \
    --print-to-pdf="$OUT" "file://$WORK/quickref.html" 2>/dev/null
[ -s "$OUT" ] || { echo "build.sh: $CHROME did not write $OUT" >&2; exit 1; }
echo "build.sh: wrote $OUT"
