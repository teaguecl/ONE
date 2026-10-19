#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Check that the emulator core writes to the console only through log.h (ctest console_output, label lint).

    tools/print_check.py

In the core's .c files, printf/puts/perror and fprintf to stdout or stderr are only allowed in the functions that
produce output the user asked for (the help text, --list-gamepads, the -D memory dump): those are listed below.
Everything else uses LOG_ERROR/LOG_WARN/LOG_INFO/LOG_DEBUG/LOG_TRACE (log.h), and the Mesen trace its own stream
(trace.c). Writing to other files (fprintf(out, ...): settings, the APU log) is fine.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP = {"log.c"}  # the logger itself
ALLOWED = {
    "one.c": {"display_help", "display_version", "display_usage", "dump_memory"},
    "input.c": {"input_list_gamepads"},
}
CALL = re.compile(r"(?<![\w.])(printf|puts|perror|vprintf)\s*\(|\bfprintf\s*\(\s*(stdout|stderr)\b|"
                  r"\bfputs\s*\([^;]*,\s*(stdout|stderr)\s*\)")
FUNCTION = re.compile(r"^(?:static\s+|inline\s+)*[A-Za-z_][\w\s\*]*?\b(\w+)\s*\([^;]*$")


def main():
    problems = []
    sources = [(n, os.path.join(ROOT, "src", n)) for n in os.listdir(os.path.join(ROOT, "src"))]
    sources += [("mappers/" + n, os.path.join(ROOT, "src", "mappers", n))
                for n in os.listdir(os.path.join(ROOT, "src", "mappers"))]
    for name, path in sorted(sources):
        if not name.endswith(".c") or name in SKIP:
            continue
        current = None
        with open(path, encoding="utf-8") as f:
            for number, line in enumerate(f, 1):
                if line.startswith("}"):
                    current = None
                    continue
                m = FUNCTION.match(line)
                if m and not line.startswith((" ", "\t", "#")):
                    current = m.group(1)
                code = line.split("//", 1)[0]
                if CALL.search(code) and current not in ALLOWED.get(name, set()):
                    problems.append(f"{name}:{number}: {line.strip()}")
    if problems:
        print("Console output outside log.h (use LOG_ERROR/LOG_WARN/LOG_INFO/LOG_DEBUG/LOG_TRACE):")
        for p in problems:
            print("  " + p)
        return 1
    print("print_check.py: the core writes to the console only through log.h (and the listed output functions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
