#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Check that every variable of the emulator core is part of the save states (ctest savestate_fields).

    tools/state_check.py [--clang CLANG] [-I DIR...] [--verbose]

-I: more include directories (the build's generated headers: cheats.c includes gamegenie_db.h).

Asks clang for every file-scope and function-scope static variable of the core's .c files (the variables that
make up the machine), and checks that each one is named in its translation unit's state function, with STATE()
(saved) or STATE_SKIP() (not machine state: a pointer to the ROM, a debugging switch, ...). A variable that is in
neither would silently be left out of save states, so the check fails. Constants are not checked.

Exit status: 0 = all listed, 1 = some are missing, 2 = clang 18 not found (ctest reports the test as skipped).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# translation unit -> the files whose variables it owns (rp2a03.c is #included into cpu.c)
UNITS = {
    "cpu.c": ["cpu.c", "rp2a03.c"],
    "ppu.c": ["ppu.c"],
    "mapper.c": ["mapper.c"],
    "mappers/mapper_nrom.c": ["mapper_nrom.c"],
    "mappers/mapper_discrete.c": ["mapper_discrete.c"],
    "mappers/mapper_mmc1.c": ["mapper_mmc1.c"],
    "mappers/mapper_txrom.c": ["mapper_txrom.c"],
    "mappers/mapper_pxrom.c": ["mapper_pxrom.c"],
    "cheats.c": ["cheats.c"],
    "memory.c": ["memory.c"],
}
LOC = re.compile(r"([A-Za-z0-9_./-]+\.(?:c|h|inc)):\d+:\d+")


def find_clang(wanted):
    for name in ([wanted] if wanted else []) + ["clang-18", "clang"]:
        path = shutil.which(name)
        if path:
            out = subprocess.run([path, "--version"], capture_output=True, text=True).stdout
            if re.search(r"version 18\.", out):
                return path
    return None


INCLUDES = []  # -I DIR (main())


def variables(clang, unit):
    """(file, line, name) of every non-constant static or global variable defined in the unit"""
    cmd = [clang, "-fsyntax-only", "-Xclang", "-ast-dump", "-fno-color-diagnostics", "-I", os.path.join(ROOT, "src"), "-I", os.path.join(ROOT, "third_party"),
           "-DDEBUGGING_ENABLED", "-DMESEN_COMPATIBILITY", "-DONE_DEBUGGER", "-DONE_HAVE_SDL2",
           "-std=gnu11", os.path.join(ROOT, "src", unit)]
    for d in INCLUDES:
        cmd[1:1] = ["-I", d]
    sdl = subprocess.run(["pkg-config", "--cflags", "sdl2"], capture_output=True, text=True)
    if sdl.returncode == 0:
        cmd[1:1] = sdl.stdout.split()
    else:
        cmd.remove("-DONE_HAVE_SDL2")
    dump = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if dump.returncode != 0:
        sys.exit(f"clang failed on {unit}:\n{dump.stderr[-2000:]}")
    current = unit
    found = []
    for line in dump.stdout.splitlines():
        is_var = "VarDecl " in line and "ParmVarDecl" not in line
        if is_var:
            # the declaration's own location follows the source range: "<range> LOCATION name 'type' ..."
            m = re.search(r"> ((?:line|col):[\d:]+|[A-Za-z0-9_./-]+:\d+:\d+) (?:used |referenced )?(\w+) '([^']*)'",
                          line)
            depth = len(line) - len(line.lstrip("| `-"))
            explicit = None
            if m:
                before = line[:m.start(1)]
                files = LOC.findall(before)
                loc = m.group(1)
                explicit = LOC.match(loc)
                file = explicit.group(1) if explicit else (files[-1] if files else current)
        files = LOC.findall(line)
        if files:
            current = files[-1]
        if not is_var or not m:
            continue
        name, vartype = m.group(2), m.group(3)
        top = depth <= 2
        static = " static" in line[m.end():]
        if " extern" in line[m.end():] or "ParmVarDecl" in line:
            continue
        if not top and not static:
            continue  # local variable
        base = re.sub(r"\[[^]]*\]$", "", vartype).strip()
        if base.startswith("const ") and "*" not in base or base.endswith("const"):
            continue  # a constant
        if os.path.basename(file) not in UNITS[unit]:
            continue
        found.append((os.path.basename(file), name))
    return found


def source_path(name):
    """a core source file's path: in src/, or in src/mappers/"""
    top = os.path.join(ROOT, "src", name)
    return top if os.path.isfile(top) else os.path.join(ROOT, "src", "mappers", name)


def listed(unit):
    text = ""
    for f in UNITS[unit]:
        with open(source_path(f), encoding="utf-8") as src:
            text += src.read()
    names = set()
    for m in re.finditer(r"STATE(?:_SKIP)?\(\s*(?:io\s*,\s*)?([A-Za-z_]\w*)", text):
        names.add(m.group(1))
    for m in re.finditer(r"state_bytes\(\s*io\s*,\s*\"([A-Za-z_]\w*)\"", text):
        names.add(m.group(1))
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clang")
    ap.add_argument("-I", dest="includes", action="append", default=[], help="more include directories")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    INCLUDES.extend(args.includes)
    clang = find_clang(args.clang)
    if clang is None:
        print("state_check.py: clang 18 not found")
        return 2
    missing = []
    total = 0
    for unit in UNITS:
        names = listed(unit)
        seen = set()
        for file, name in variables(clang, unit):
            if (file, name) in seen:
                continue
            seen.add((file, name))
            total += 1
            if args.verbose:
                print(f"{file}: {name}{'' if name in names else '   <-- MISSING'}")
            if name not in names:
                missing.append(f"{file}: {name}")
    if missing:
        print("Variables in neither STATE() nor STATE_SKIP() (they would be left out of save states):")
        for m in missing:
            print("  " + m)
        return 1
    print(f"state_check.py: all {total} variables of the core are listed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
