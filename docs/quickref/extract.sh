#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Pull the quick reference's facts out of the debugger source with grep/sed/awk.
#   extract.sh SOURCE_DIR OUT_DIR
# Writes HTML fragments into OUT_DIR, one per "<!-- @include NAME -->" placeholder in quickref.md:
#   commands.html   all commands, grouped by the "// @group" lines of the command table (debugger/commands.cpp)
#   essential.html  full help text of the commands named by the @essential line in quickref.md
#   keys.html       function keys (the menus, debugger/imgui/app.cpp)
#   keybar.html     the status line as a key bar (debugger/imgui/app.cpp)
#   windows.html    the windows and their numbers (debugger/imgui/app.cpp, app.hpp)
#   options.html    command-line options (usage() in debugger/main.cpp)
#   script.html     script-only commands (debugger/commands.cpp)
set -euo pipefail
SRC=${1:?usage: extract.sh SOURCE_DIR OUT_DIR}
OUT=${2:?usage: extract.sh SOURCE_DIR OUT_DIR}
DBG=$SRC/debugger
mkdir -p "$OUT"

# HTML-escape stdin
esc() { sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'; }

# ---- the command table: one record per command: group <TAB> name <TAB> help line 1 <TAB> help line 2 ... --------
# Table lines look like:  {"bpx", &CommandProcessor::cmdBpx, {"Breakpoint on execution.  Syntax: bpx address", ..., nullptr}},
awk '
    /static const std::vector<Command> table = \{/ { inside = 1; next }
    inside && /^[ \t]*\};/                     { inside = 0 }
    !inside                                    { next }
    /\/\/ @group /                             { sub(/.*\/\/ @group /, ""); group = $0; next }
    /^[ \t]*\{"/ {
        line = $0; rec = group
        while (match(line, /"([^"\\]|\\.)*"/)) {          # every string literal, in order
            s = substr(line, RSTART + 1, RLENGTH - 2)
            gsub(/\\"/, "\"", s)
            rec = rec "\t" s
            line = substr(line, RSTART + RLENGTH)
        }
        print rec
    }' "$DBG/commands.cpp" > "$OUT/commands.tsv"

# commands.html: aliases ("Same as g", or the same first help line as the previous command) share one entry
esc < "$OUT/commands.tsv" | awk -F'\t' '
    function flush() {
        if (names == "") return
        printf "<div class=\"cmd\"><span class=\"name\">%s</span><span class=\"key\">%s</span><span class=\"desc\">%s</span></div>\n", names, key, desc
        names = ""
    }
    {
        group = $1; name = toupper($2); help = $3
        if (group != lastGroup) {
            flush()
            if (lastGroup != "") print "</div>"
            printf "<div class=\"group\"><h3>%s</h3>\n", group
            lastGroup = group
        }
        if (names != "" && (help ~ /^Same as / || help == lastHelp)) { names = names ", " name; next }
        flush()
        names = name; lastHelp = help
        # "Description.  Syntax: x y" -> description and syntax
        desc = help; syntax = ""
        if (match(help, /  Syntax: /)) { desc = substr(help, 1, RSTART - 1); syntax = substr(help, RSTART + RLENGTH) }
        sub(/\.$/, "", desc)
        # function key from a "Func key: [F8] ..." help line
        key = ""
        for (i = 4; i <= NF; i++) if ($i ~ /^Func key: \[/) { key = $i; sub(/^Func key: \[/, "", key); sub(/\].*/, "", key) }
        if (syntax != "") desc = desc " <code>" syntax "</code>"     # (the stylesheet puts the syntax on a line of its own)
    }
    END { flush(); if (lastGroup != "") print "</div>" }' > "$OUT/commands.html"

# essential.html: the commands listed on the "@essential" line of quickref.md, with all their help lines
essential=$(grep -E '^ *@essential ' "$SRC/docs/quickref/quickref.md" | head -1 | sed -e 's/^ *@essential //')
for name in $essential; do
    awk -F'\t' -v want="$name" '$2 == want' "$OUT/commands.tsv" | head -1
done | esc | awk -F'\t' '{
        printf "<div class=\"ess\"><div class=\"name\">%s</div><div class=\"desc\">", toupper($2)
        for (i = 3; i <= NF; i++) {
            s = $i; sub(/^ +/, "", s)
            if (match(s, /  Syntax: /)) s = substr(s, 1, RSTART - 1) " <code>" substr(s, RSTART + RLENGTH) "</code>"
            printf "%s%s", (i > 3 ? "<br>" : ""), s
        }
        print "</div></div>"
    }' > "$OUT/essential.html"

# ---- keys: the menus' items carry a key text and a command; the status line is a list of "~F5~ Go" strings ----------
#   {"~G~o / stop", "F5", cmGo}        kStatusItems[] = {"~F1~ Help", "~F5~ Go", ...}      (debugger/imgui/app.cpp)
APP=$DBG/imgui/app.cpp
grep -o '{"[^"]*", *"[^"]*", *cm[A-Za-z]*}' "$APP" |
    sed -E 's/\{"([^"]*)", *"([^"]*)", *(cm[A-Za-z]*)\}/\3\t\2\t\1/' | sed -e 's/~//g' |
    awk -F'\t' '$2 != "" && !seen[$2]++ { printf "<div class=\"kv\"><span class=\"key\">%s</span><span>%s</span></div>\n", $2, $3 }' \
    > "$OUT/keys.html"

# keybar.html: the visible status line items, "~F5~ Go" -> F5: GO. Sorted: the function keys in ascending order,
# then the other keys in the order of the status line ("~Ctrl+D~ Break", "~Alt+X~ Exit").
awk '/kStatusItems\[\] *= *\{/ { inside = 1; next } inside && /^\};/ { inside = 0 } inside' "$APP" |
    grep -o '"~[^"]*~ [^"]*"' | tr -d '"' |
    awk '{
        split($0, part, "~"); key = part[2]; label = part[3]; sub(/^ +/, "", label)
        order = (key ~ /^F[0-9]+$/) ? substr(key, 2) + 0 : 100 + NR
        printf "%d\t<span><b>%s:</b> %s</span>\n", order, key, toupper(label)
    }' | sort -n -k1,1 | cut -f2- > "$OUT/keybar.html"

# windows.html: the numbers are the PaneId enum's order (app.hpp), the names panes[kRegisters].title = "Registers"
HPP=$DBG/imgui/app.hpp
grep -o 'enum PaneId {[^}]*}' "$HPP" | sed -e 's/.*{//' -e 's/}//' -e 's/=[^,]*//g' | tr ',' '\n' | tr -d ' ' |
    awk '{ print NR - 1 "\t" $0 }' | while IFS=$'\t' read -r number id; do
        title=$(grep -o "panes\\[$id\\]\\.title = \"[^\"]*\"" "$APP" | sed -e 's/.*= "//' -e 's/"$//' | head -1 || true)
        if [ -n "$title" ]; then printf '%s\t%s\n' "$number" "$title"; fi
    done | sort -n |
    awk -F'\t' '{ printf "<div class=\"kv\"><span class=\"num\">%s</span><span>%s window</span></div>\n", $1, $2 }' \
    > "$OUT/windows.html"

# ---- options: the usage() text in main.cpp --------------------------------------------------------------------------
#   "  -l, --labels FILE   load labels (ld65 -Ln / VICE format)\n"
awk '/^void usage\(\)/ { inside = 1 } inside && /^}/ { inside = 0 } inside' "$DBG/main.cpp" |
    grep -o '"  [^"]*\\n"' | sed -e 's/^"//' -e 's/\\n"$//' | esc |
    sed -E 's/^ +((-[a-zA-Z], )?--[a-z-]+( [A-Z]+)?) +(.*)$/<div class="kv"><span class="opt">\1<\/span><span>\4<\/span><\/div>/' \
    > "$OUT/options.html"

# ---- script commands: host.print("Script commands: !assert reg r value, ...") ----------------------------------------
grep -o '"Script commands: [^"]*"' "$DBG/commands.cpp" | head -1 | sed -e 's/^"Script commands: //' -e 's/"$//' |
    tr ',' '\n' | sed -e 's/^ *//' | esc |
    awk '{ printf "<div class=\"kv\"><code>%s</code></div>\n", $0 }' > "$OUT/script.html"

# a summary for the build log
printf 'extract.sh: %d commands, %d keys, %d windows, %d options\n' \
    "$(wc -l < "$OUT/commands.tsv")" "$(grep -c 'class="kv"' "$OUT/keys.html")" \
    "$(grep -c 'class="kv"' "$OUT/windows.html")" "$(grep -c 'class="kv"' "$OUT/options.html")"
