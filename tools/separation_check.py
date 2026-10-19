#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Check the wall between the emulator and its menu (ctest gui_separation, labels smoke and lint).

    tools/separation_check.py

The menu (gui/, C++ with Dear ImGui) may use the emulator only through frontend.h (and settings.h, which
frontend.h includes): no other header of the core may be included in gui/. The other way round, the core includes
nothing from gui/ except gui/gui.h in one.c (main() hands the menu to the emulator), and neither the core nor the
debugger's shared files include Dear ImGui (only gui/ and one-debugger's ImGui user interface, debugger/imgui/, do;
main.cpp includes that one's own imgui/app.hpp). gui/gui_test.cpp, the menu's test, may also include the headers it needs to start a
window and a game.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')

GUI_MAY_INCLUDE = {"frontend.h", "settings.h"}
GUI_TEST_MAY_ALSO_INCLUDE = {"cpu.h", "emu.h", "input.h", "png.h", "savestate.h", "scheduler.h", "video.h"}
CORE_MAY_INCLUDE_FROM_GUI = {"one.c": {"gui/gui.h"}}


def includes(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        for number, line in enumerate(f, 1):
            m = INCLUDE.match(line)
            if m:
                yield number, m.group(1)


def main():
    src_dir = os.path.join(ROOT, "src")
    core_headers = {name for name in os.listdir(src_dir) if name.endswith(".h")}
    core_headers |= {"mappers/" + name for name in os.listdir(os.path.join(src_dir, "mappers")) if name.endswith(".h")}
    gui_dir = os.path.join(ROOT, "gui")
    gui_files = sorted(n for n in os.listdir(gui_dir) if n.endswith((".cpp", ".hpp", ".h")))
    problems = []

    for name in gui_files:
        allowed = GUI_MAY_INCLUDE | (GUI_TEST_MAY_ALSO_INCLUDE if name.endswith("_test.cpp") else set())
        for number, header in includes(os.path.join(gui_dir, name)):
            if header in core_headers and header not in allowed:
                problems.append(f"gui/{name}:{number}: includes the core's {header} (the menu may use frontend.h only)")

    others = [(name, os.path.join(src_dir, name)) for name in os.listdir(src_dir) if name.endswith((".c", ".h"))]
    others += [("mappers/" + n, os.path.join(src_dir, "mappers", n))
               for n in os.listdir(os.path.join(src_dir, "mappers"))
               if n.endswith((".c", ".h"))]
    debugger_dir = os.path.join(ROOT, "debugger")
    others += [("debugger/" + n, os.path.join(debugger_dir, n)) for n in os.listdir(debugger_dir)
               if n.endswith((".cpp", ".hpp", ".h"))]
    for name, path in sorted(others):
        for number, header in includes(path):
            if header.startswith("gui/") and header not in CORE_MAY_INCLUDE_FROM_GUI.get(name, set()):
                problems.append(f"{name}:{number}: includes {header} (only one.c may include gui/gui.h)")
            ours = name.startswith("debugger/") and os.path.isfile(os.path.join(debugger_dir, header))
            if (header.startswith("imgui") or header.startswith("backends/")) and not ours:
                problems.append(f"{name}:{number}: includes Dear ImGui's {header} "
                                "(only gui/ and debugger/imgui/ use Dear ImGui)")

    if problems:
        print("The menu and the emulator are not kept apart:")
        for p in problems:
            print("  " + p)
        return 1
    print(f"separation_check.py: gui/ ({len(gui_files)} files) uses the emulator through frontend.h only")
    return 0


if __name__ == "__main__":
    sys.exit(main())
