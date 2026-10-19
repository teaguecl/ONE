#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Disable Sprite Limit (video.disable_sprite_limit, ctest debugger_sprite_limit): test_roms/obj_over.nes puts nine
sprites on one line. With the setting off the picture shows the hardware's eight; on, the ninth is there too and
nothing else changes (every difference lies in the sprites' rows).

    tests/sprite_limit_check.py ONE-DEBUGGER obj_over.nes SCRATCH_DIR
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bg_check import read_png  # noqa: E402


def shot(debugger, rom, scratch, setting):
    cfg = os.path.join(scratch, f"sl_{setting}.cfg")
    script = os.path.join(scratch, f"sl_{setting}.txt")
    png = os.path.join(scratch, f"sl_{setting}.png")
    with open(cfg, "w") as f:
        f.write(f"video.disable_sprite_limit = {setting}\n")
    with open(script, "w") as f:
        f.write(f"fs 120\nscreenshot {png}\n")
    env = {k: v for k, v in os.environ.items() if k != "ONE_HARDWARE_SPRITES"}
    env.update(SDL_VIDEODRIVER="dummy", XDG_CONFIG_HOME=scratch, XDG_STATE_HOME=scratch)
    out = subprocess.run([debugger, "--batch", "--no-init", "--one-config", cfg, "-s", script, rom],
                         capture_output=True, text=True, env=env, timeout=100).stdout
    return read_png(png, None), out


def main():
    debugger, rom, scratch = sys.argv[1:4]
    off, off_out = shot(debugger, rom, scratch, "off")
    on, on_out = shot(debugger, rom, scratch, "on")
    if "Disable Sprite Limit is on" in off_out or "Disable Sprite Limit is on" not in on_out:
        print("sprite_limit: the warning at startup is wrong")
        return 1
    diff = [(x, y) for y in range(len(off)) for x in range(len(off[0])) if off[y][x] != on[y][x]]
    if not diff:
        print("sprite_limit: the setting changes nothing")
        return 1
    rows = {y for _, y in diff}
    if max(rows) - min(rows) > 15:
        print(f"sprite_limit: differences in rows {min(rows)}-{max(rows)}: more than one sprite")
        return 1
    print(f"sprite_limit: ok ({len(diff)} pixels more, rows {min(rows)}-{max(rows)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
