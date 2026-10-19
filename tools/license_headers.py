#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
"""Give every first-party source file the GPL-2.0-or-later notice and an SPDX line.

    python3 tools/license_headers.py [--check]

Files that have the long notice (C/C++) keep their years; the old "59 Temple Place" address is replaced by the
FSF's current pointer. Files without one get the notice (C/C++) or a short form (scripts, CMake, Lua). --check only
lists the files that need a change (exit status 1). Third-party files are skipped (THIRD_PARTY below).
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPDX = "SPDX-License-Identifier: GPL-2.0-or-later"
YEARS = "1999-2026"
HOLDER = "Chris Teague"

# not ours: libco (ISC), libretro.h (MIT), the Lua scripts of others, downloaded test ROMs and their sources
SKIP_DIRS = {"build", "commercial_games", "test_roms", "lua_scripts", "libco", ".git",
             "__pycache__"}
SKIP_FILES = {"libretro/libretro.h"}
C_EXT = (".c", ".h", ".cpp", ".hpp")
HASH_EXT = (".py", ".sh", ".cmake")
LUA_EXT = (".lua",)

LONG = """  ONE NES emulator
  Copyright (C) {years} {holder}

  ONE is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  ONE is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with ONE; if not, see <https://www.gnu.org/licenses/>."""

SHORT = "Copyright (C) {years} {holder}\nPart of ONE, a NES emulator: GPL, version 2 or later (see LICENSE)."


def files():
    for top, dirs, names in os.walk(ROOT):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS and not d.startswith(".") and d != "_deps")
        for name in sorted(names):
            path = os.path.join(top, name)
            rel = os.path.relpath(path, ROOT)
            if rel in SKIP_FILES or name.endswith("~") or name.startswith(("#", ".#")):
                continue
            if name.endswith(C_EXT + HASH_EXT + LUA_EXT) or name == "CMakeLists.txt":
                yield rel, path


def c_new(text):
    m = re.match(r"/\*(.*?)\*/", text, re.S)
    if m and "GNU General Public License" in m.group(1):
        body = m.group(1)
        years = re.search(r"Copyright \(C\) (\S+) " + HOLDER, body)
        years = years.group(1) if years else YEARS
        if SPDX in body:
            old_free = "ONE is free software" in body
            new = body
        else:
            new = "\n  " + SPDX + "\n" + re.sub(r"^\n", "", body)
        new = new.replace("beastie", "ONE")
        new = re.sub(r"along with ONE; if not, write to the Free Software\n\s*Foundation, Inc\.,[^\n]*",
                     "along with ONE; if not, see <https://www.gnu.org/licenses/>.", new)
        return "/*" + new + "*/" + text[m.end():]
    return "/*\n  " + SPDX + "\n" + LONG.format(years=YEARS, holder=HOLDER) + "\n*/\n" + text


def script_new(text, comment):
    if SPDX in text[:600]:
        return text
    lines = (SPDX + "\n" + SHORT.format(years=YEARS, holder=HOLDER)).split("\n")
    block = "".join(comment + " " + l + "\n" for l in lines)
    if text.startswith("#!"):
        first, _, rest = text.partition("\n")
        return first + "\n" + block + rest
    return block + text


def new_text(rel, text):
    if rel.endswith(C_EXT):
        return c_new(text)
    if rel.endswith(LUA_EXT):
        return script_new(text, "--")
    return script_new(text, "#")


def main():
    check = "--check" in sys.argv
    changed = []
    for rel, path in files():
        with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
            text = f.read()
        if re.search(r"Copyright[^\n]*", text[:2000]) and HOLDER not in text[:2000] and "Copyright" in text[:2000]:
            continue  # someone else's notice: not ours to change
        new = new_text(rel, text)
        if new != text:
            changed.append(rel)
            if not check:
                with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                    f.write(new)
    for rel in changed:
        print(rel)
    print(f"license_headers.py: {len(changed)} files {'need a header' if check else 'changed'}")
    return 1 if check and changed else 0


if __name__ == "__main__":
    sys.exit(main())
