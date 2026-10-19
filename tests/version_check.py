#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""One version everywhere (ctest version_consistency, labels smoke and lint).

    tests/version_check.py --project-version 0.1.0 --one build/one [--debugger build/debugger/one-debugger]
                           [--info libretro/one_libretro.info]
                           [--libretro-host build/tests/libretro_host --core build/one_libretro.so]

The version is set once, in CMakeLists.txt (project VERSION). Every program that prints one (`one --version`,
`one-debugger --version`, the libretro core's system info) must print that number, optionally followed by the commit
(`+g1a2b3c4`, and `-dirty` for local changes: cmake/version.cmake), and the static libretro/one_libretro.info must
carry the same number in display_version.
"""
import argparse
import re
import subprocess
import sys


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--project-version', required=True)
    ap.add_argument('--one', required=True)
    ap.add_argument('--debugger')
    ap.add_argument('--info')
    ap.add_argument('--libretro-host')
    ap.add_argument('--core')
    args = ap.parse_args()

    base = args.project_version
    pattern = re.compile(r'(?<![\w.+-])' + re.escape(base) + r'(\+g[0-9a-f]{7}(-dirty)?)?(?![\w.+-])')
    failures = []
    found = []

    def check(what, text, first_line_only=True):
        line = text.strip().splitlines()[0] if first_line_only and text.strip() else text
        m = pattern.search(line)
        if not m:
            failures.append('%s: expected version %s[+gHASH[-dirty]], got %r' % (what, base, line.strip()))
        else:
            found.append(m.group(0))

    check('one --version', run([args.one, '--version']))
    if args.debugger:
        check('one-debugger --version', run([args.debugger, '--version']))
    if args.libretro_host:
        out = run([args.libretro_host, args.core, '--version'])
        m = re.search(r'^libretro_host: core ONE (\S+)', out, re.M)
        if not m:
            failures.append('libretro core: no "libretro_host: core ONE <version>" line')
        else:
            check('libretro core', m.group(1))
    if args.info:
        m = re.search(r'^display_version\s*=\s*"([^"]*)"', open(args.info).read(), re.M)
        if not m or m.group(1) != base:
            failures.append('%s: display_version is %r, project version is %s' % (args.info, m and m.group(1), base))

    # all the programs of one build say the same thing, commit included
    if len(set(found)) > 1:
        failures.append('the programs disagree: %s' % sorted(set(found)))
    for f in failures:
        print('FAIL', f)
    if failures:
        sys.exit(1)
    print('version_check: %s everywhere (%s)' % (base, found[0] if found else base))


if __name__ == '__main__':
    main()
