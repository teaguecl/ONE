#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# clang-format for ONE's own C and C++ sources (the style is in .clang-format).
#
#   tools/format.sh                   reformat every source file in place
#   tools/format.sh --check           change nothing; list the files that are not formatted, exit 1 if any
#   tools/format.sh [--check] FILE…   only these files
#   tools/format.sh --check --staged  check what is staged for the next git commit (the pre-commit hook)
#
# Also run by:  cmake --build build --target format / format-check,
#               ctest (test clang_format, labels smoke and lint),  and tools/pre-commit.
# ONE's own sources are src/ (*.c *.h, mappers/), debugger/ and gui/ (*.cpp *.hpp, gui/*.h) and tests/*.c; third-party
# code (third_party/) and build trees are left alone.
# clang-format's output differs a little between major versions, so the version is checked;
# CLANG_FORMAT=/path/to/clang-format picks the binary.  Exit status 2: no suitable clang-format.
set -euo pipefail

required_major=18
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

check=false
staged=false
while [ $# -gt 0 ]; do
    case "$1" in
    --check) check=true ;;
    --staged) staged=true ;;
    -*) echo "format.sh: unknown option $1" >&2; exit 2 ;;
    *) break ;;
    esac
    shift
done
if $staged && ! $check; then
    echo "format.sh: --staged only checks (use --check --staged)" >&2
    exit 2
fi

clang_format=${CLANG_FORMAT:-}
if [ -z "$clang_format" ]; then
    for candidate in "clang-format-$required_major" clang-format; do
        if command -v "$candidate" >/dev/null 2>&1; then
            clang_format=$candidate
            break
        fi
    done
fi
if [ -z "$clang_format" ] || ! command -v "$clang_format" >/dev/null 2>&1; then
    echo "format.sh: clang-format not found (Debian/Ubuntu/Mint: sudo apt install clang-format-$required_major)" >&2
    exit 2
fi
major=$( ("$clang_format" --version 2>/dev/null || true) | sed -n 's/.*version \([0-9]*\).*/\1/p')
if [ "$major" != "$required_major" ]; then
    echo "format.sh: $clang_format is version $major; ONE's formatting is defined by clang-format $required_major" >&2
    echo "           (sudo apt install clang-format-$required_major, or set CLANG_FORMAT=...)" >&2
    exit 2
fi

# ONE's own sources (paths relative to the top directory)
is_own_source() {
    [[ "$1" =~ ^[^/]+\.(c|h)$ || "$1" =~ ^(debugger|debugger/imgui|gui)/[^/]+\.(cpp|hpp|h)$ || "$1" =~ ^tests/[^/]+\.c$ ]]
}

cd "$root"
files=()
if [ $# -gt 0 ]; then
    files=("$@")
elif $staged; then
    while IFS= read -r file; do
        if is_own_source "$file"; then
            files+=("$file")
        fi
    done < <(git diff --cached --name-only --diff-filter=ACMR)
else
    shopt -s nullglob
    for file in src/*.c src/*.h src/mappers/*.c src/mappers/*.h debugger/*.cpp debugger/*.hpp debugger/imgui/*.cpp debugger/imgui/*.hpp gui/*.cpp gui/*.hpp gui/*.h \
        tests/*.c; do
        files+=("$file")
    done
fi

status=0
for file in "${files[@]}"; do
    if $staged; then
        # the staged content is what gets committed, whatever the working tree holds
        if ! cmp -s <(git show ":$file") <(git show ":$file" | "$clang_format" --style=file --assume-filename="$file"); then
            echo "not formatted: $file"
            status=1
        fi
    elif $check; then
        if ! "$clang_format" --style=file "$file" | cmp -s - "$file"; then
            echo "not formatted: $file"
            status=1
        fi
    else
        "$clang_format" --style=file -i "$file"
    fi
done
if [ $status -ne 0 ]; then
    if $staged; then
        echo "Fix with: tools/format.sh FILE... && git add FILE...   (or commit anyway with git commit --no-verify)" >&2
    else
        echo "Fix with: tools/format.sh   (or: cmake --build build --target format)" >&2
    fi
fi
exit $status
