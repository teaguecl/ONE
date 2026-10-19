#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Builds the RAMBO-1 IRQ test ROMs (see rambo1.s): rambo1_<mode>.nes
cd "$(dirname "$0")" || exit 1
for m in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17; do
    ca65 -D MODE=$m rambo1.s -o /tmp/rambo1_$m.o && ld65 -C rambo1.cfg -o rambo1_$m.nes /tmp/rambo1_$m.o || exit 1
done
for m in 18 19 20 21; do
    ca65 -D MODE=$((m - 8)) -D VBLANK=1 rambo1.s -o /tmp/rambo1_$m.o && ld65 -C rambo1.cfg -o rambo1_$m.nes /tmp/rambo1_$m.o || exit 1
done
