#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# clang-tidy for ONE's own C and C++ sources (the checks are in .clang-tidy, debugger/.clang-tidy and gui/.clang-tidy).
#
#   tools/tidy.sh [-p BUILD_DIR] [--fix] [FILE...]
#
#   -p BUILD_DIR  a CMake build directory (it has compile_commands.json; any configured build will do).
#                 Default: build/linux-debugger, build/linux-release, build/linux-debug or build, the first that has
#                 one. A build with the debugger checks the debugger too.
#   --fix         apply the fixes clang-tidy offers (one file at a time; review them with a diff afterwards)
#   FILE...       only these files; a header checks the sources that #include it.  Default: all sources.
#
# Also run by:  cmake --build build --target tidy,  ctest -L lint (test clang_tidy),  and tools/pre-commit.
# The core is compiled twice, with and without the debugger hooks (ONE_DEBUGGER); both versions are checked.
# Third-party code (third_party/, SDL) is not checked.
# Needs clang-tidy 18 (other versions have other checks); CLANG_TIDY=/path/to/clang-tidy picks the binary.
# Exit status: 0 clean, 1 findings, 2 no suitable clang-tidy or no compile_commands.json.
set -euo pipefail

required_major=18
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

build_dir=
fix=false
while [ $# -gt 0 ]; do
    case "$1" in
    -p) build_dir=${2:?-p needs a build directory}; shift ;;
    --fix) fix=true ;;
    -*) echo "tidy.sh: unknown option $1" >&2; exit 2 ;;
    *) break ;;
    esac
    shift
done

clang_tidy=${CLANG_TIDY:-}
if [ -z "$clang_tidy" ]; then
    for candidate in "clang-tidy-$required_major" clang-tidy; do
        if command -v "$candidate" >/dev/null 2>&1; then
            clang_tidy=$candidate
            break
        fi
    done
fi
if [ -z "$clang_tidy" ] || ! command -v "$clang_tidy" >/dev/null 2>&1; then
    echo "tidy.sh: clang-tidy not found (Debian/Ubuntu/Mint: sudo apt install clang-tidy-$required_major)" >&2
    exit 2
fi
major=$( ("$clang_tidy" --version 2>/dev/null || true) | sed -n 's/.*version \([0-9]*\).*/\1/p' | head -1)
if [ "$major" != "$required_major" ]; then
    echo "tidy.sh: $clang_tidy is version $major; ONE's checks are set up for clang-tidy $required_major" >&2
    echo "         (sudo apt install clang-tidy-$required_major, or set CLANG_TIDY=...)" >&2
    exit 2
fi

cd "$root"
if [ -z "$build_dir" ]; then
    for candidate in build/linux-debugger build/linux-release build/linux-debug build; do
        if [ -f "$candidate/compile_commands.json" ]; then
            build_dir=$candidate
            break
        fi
    done
fi
if [ -z "$build_dir" ] || [ ! -f "$build_dir/compile_commands.json" ]; then
    echo "tidy.sh: no compile_commands.json; configure a build first (cmake --preset linux-debugger) or use -p DIR" >&2
    exit 2
fi
build_dir=$(cd "$build_dir" && pwd)

# Two compile databases with ONE's own sources only: 'main' (the debugger build of the core when there is one,
# plus the debugger and the tools) and 'plain' (the core without the debugger hooks, when it is built both ways).
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/main" "$work/plain"
python3 - "$build_dir/compile_commands.json" "$root" "$work" <<'EOF'
import json, os, re, sys
db, root, work = sys.argv[1], os.path.realpath(sys.argv[2]), sys.argv[3]
own = re.compile(r'^(src/(mappers/)?[^/]+\.c|(debugger|debugger/imgui|gui)/[^/]+\.cpp|tests/[^/]+\.c)$')
entries = {}
for e in json.load(open(db)):
    path = os.path.realpath(os.path.join(e['directory'], e['file']))
    rel = os.path.relpath(path, root)
    if not own.match(rel):
        continue
    plain = 'one_core.dir' in e.get('command', ' '.join(e.get('arguments', [])))
    entries.setdefault(rel, {})['plain' if plain else 'main'] = e
main, plain = [], []
for rel, variants in sorted(entries.items()):
    if 'main' in variants:
        main.append(variants['main'])
        if 'plain' in variants:
            plain.append(variants['plain'])
    else:
        main.append(variants['plain'])
json.dump(main, open(os.path.join(work, 'main', 'compile_commands.json'), 'w'))
json.dump(plain, open(os.path.join(work, 'plain', 'compile_commands.json'), 'w'))
for name, lst in (('main', main), ('plain', plain)):
    with open(os.path.join(work, name + '.files'), 'w') as f:
        for e in lst:
            f.write(os.path.relpath(os.path.realpath(os.path.join(e['directory'], e['file'])), root) + '\n')
EOF

# Which files: all, or the given ones (a header stands for the sources that include it)
wanted=$work/wanted
if [ $# -eq 0 ]; then
    cat "$work/main.files" > "$wanted"
else
    : > "$wanted"
    for file in "$@"; do
        case "$file" in
        *.h | *.hpp)
            grep -l "#include \"$(basename "$file")\"" -- src/*.c src/mappers/*.c debugger/*.cpp debugger/imgui/*.cpp gui/*.cpp tests/*.c 2>/dev/null >> "$wanted" || true ;;
        *) echo "$file" >> "$wanted" ;;
        esac
    done
    sort -u -o "$wanted" "$wanted"
    while IFS= read -r file; do
        if ! grep -qFx -- "$file" "$work/main.files" "$work/plain.files"; then
            echo "tidy.sh: not checked, not part of the build in $build_dir: $file" >&2
        fi
    done < "$wanted"
fi

# Headers of ONE itself are checked too (not those of SDL, libco)
header_filter="^$(printf '%s' "$root" | sed 's/[][\.*^$+?(){}|]/\\&/g')/(src/|src/mappers/|debugger/|debugger/imgui/|gui/)?[^/]+\\.(h|hpp)\$"
args=(--quiet "--header-filter=$header_filter")
if $fix; then
    args+=(--fix)
    jobs=1      # fixes to a shared header must not race
else
    jobs=$(nproc 2>/dev/null || echo 4)
fi

status=0
for variant in main plain; do
    list=$work/$variant.todo
    grep -Fxf "$wanted" "$work/$variant.files" > "$list" || true
    [ -s "$list" ] || continue
    # each file: clang-tidy -p DB ARGS FILE, its "N warnings generated" chatter dropped
    xargs -a "$list" -P "$jobs" -I{} sh -c '
        tidy=$1; db=$2; file=$3; shift 3
        "$tidy" -p "$db" "$@" "$file" 2>&1 | grep -v "warnings\{0,1\} generated\.\|^Suppressed [0-9]* warnings" || true
    ' sh "$clang_tidy" "$work/$variant" {} "${args[@]}" > "$work/$variant.out"
    if [ "$variant" = plain ] && [ -s "$work/$variant.out" ]; then
        echo "--- the same, compiled without the debugger hooks (as in 'one'): ---"
    fi
    cat "$work/$variant.out"
    if grep -q ': \(warning\|error\):' "$work/$variant.out"; then
        status=1
    fi
done
if [ $status -ne 0 ]; then
    echo "clang-tidy found problems (above). Fix them, or silence one with // NOLINT(check-name) and a reason." >&2
fi
exit $status
