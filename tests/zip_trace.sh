#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# The trace of a game out of a ZIP is the trace of the .nes file (ctest zip_trace).   zip_trace.sh ONE ROOT
ONE=$1; cd "$2" || exit 2
a=$("$ONE" --headless -t -k 20000 tests/data/nestest.nes 2>&1 >/dev/null | md5sum)
b=$("$ONE" --headless -t -k 20000 tests/zip/nestest_deflate.zip 2>&1 >/dev/null | md5sum)
c=$("$ONE" --headless -t -k 20000 tests/zip/nestest_stored.zip 2>&1 >/dev/null | md5sum)
[ "$a" = "$b" ] && [ "$a" = "$c" ] && echo "zip_trace identical"
