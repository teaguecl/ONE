#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
#
# Fetch the third-party test ROMs the accuracy tests use into test_roms/ (never committed: they have their own
# licenses). Clones christopherpow/nes-test-roms to test_roms/nes-test-roms and links the folders the tests expect
# (test_roms/ppu_vbl_nmi -> test_roms/nes-test-roms/ppu_vbl_nmi ...). The Mesen traces the regression compares with
# (*_mesen.txt) are not part of any download: capture them with Mesen (tools/mesen/README.md); tests without
# their trace are skipped.
#
#   bash tools/fetch_test_roms.sh
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
mkdir -p test_roms
if [ -d test_roms/nes-test-roms/.git ]; then
    git -C test_roms/nes-test-roms pull --ff-only
else
    git clone --depth 1 https://github.com/christopherpow/nes-test-roms.git test_roms/nes-test-roms
fi
for dir in apu_test blargg_nes_cpu_test5 blargg_ppu_tests_2005.09.15b branch_timing_tests cpu_dummy_reads \
    cpu_timing_test6 dmc_dma_during_read4 dmc_tests instr_misc instr_test-v5 instr_timing MMC1_A12 oam_read \
    oam_stress ppu_open_bus ppu_read_buffer ppu_vbl_nmi sprdma_and_dmc_dma sprite_overflow_tests; do
    if [ -e "test_roms/$dir" ]; then
        continue
    fi
    if [ -d "test_roms/nes-test-roms/$dir" ]; then
        ln -s "nes-test-roms/$dir" "test_roms/$dir"
    else
        echo "fetch_test_roms.sh: $dir is not in nes-test-roms any more" >&2
    fi
done
cat <<'MSG'
Not in nes-test-roms (get them yourself if you want those tests; the tests are skipped without them):
  test_roms/AccuracyCoin/       AccuracyCoin.nes, by 100thCoin (https://github.com/100thCoin/AccuracyCoin)
  test_roms/ppu_sprite_hit/     blargg's ppu_sprite_hit tests (rom_singles/*.nes)
  test_roms/apu_reference/      Mesen recordings of the APU (tools/mesen/README.md), made by you
MSG
