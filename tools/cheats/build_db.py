#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""Build a Game Genie code database for ONE from libretro-database.

    tools/cheats/build_db.py [-o FILE] [LIBRETRO_DATABASE_DIR]

Without a directory it clones https://github.com/libretro/libretro-database (only the NES files) into a
temporary directory. The output goes to FILE, by default nes_gamegenie.txt in the "cheats" folder of ONE's
config directory (~/.config/one/cheats/, or $XDG_CONFIG_HOME/one/cheats/, or %APPDATA%\\one\\cheats\\), where
ONE reads it (the debugger's "gg db", the menu's Game Genie page). ONE does not ship this database: libretro-database is
CC-BY-SA-4.0, which the GPL does not allow ONE to include. The output is that license's adaptation of it: keep it
out of ONE's repository and releases.

Games are identified by the SHA-1 of the ROM data without the 16-byte iNES header (PRG + CHR): the same for
every dump of a game whatever its header or file name. libretro's NES list
(metadat/no-intro/Nintendo - Nintendo Entertainment System.dat) gives each No-Intro name its sha1 of that data
(its ".unh" entries; the ".nes" ones hash No-Intro's header too).

The cheats: cht/Nintendo - Nintendo Entertainment System/<No-Intro name>[ (Game Genie)|...].cht, RetroArch's format
(cheatN_desc, cheatN_code). A file counts for a game when its name, without a trailing "(Game Genie)",
"(Action Replay)" ... part, is the game's No-Intro name (allowing for RetroArch's file-name characters and region
order), or failing that when its title is that of exactly one No-Intro game: codes for another region or revision
usually do not work, so nothing looser. Only Game Genie codes are kept (6 or 8 letters; several joined by '+' are one cheat); raw RAM codes
("0347:01", Pro Action Rocky) are left for later.

Output (plain text, one game after another):
    # comments
    @<sha1 of PRG+CHR> <No-Intro name>
    <code>[+<code>...]<TAB><description>
"""
import html
import os
import re
import subprocess
import sys
import tempfile



def default_out():
    """where ONE looks for the database (settings_user_dir() in settings.c, plus "cheats")"""
    if os.name == "nt":
        base = os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")), "one")
    else:
        xdg = os.environ.get("XDG_CONFIG_HOME")
        base = os.path.join(xdg if xdg else os.path.join(os.path.expanduser("~"), ".config"), "one")
    return os.path.join(base, "cheats", "nes_gamegenie.txt")


OUT = default_out()
SYSTEM = "Nintendo - Nintendo Entertainment System"
GG = re.compile(r"^[APZLGITYEOXUKSVN]{6}([APZLGITYEOXUKSVN]{2})?$")
SUFFIX = re.compile(r"\s*\((Game Genie|Action Replay|Pro Action Replay|Pro Action Rocky|Rumbles|Codes)\)$", re.I)


def clone(where):
    subprocess.run(["git", "clone", "--depth", "1", "--filter=blob:none", "--sparse",
                    "https://github.com/libretro/libretro-database.git", where], check=True)
    subprocess.run(["git", "-C", where, "sparse-checkout", "set", "--no-cone", f"/cht/{SYSTEM}/",
                    f"/metadat/no-intro/{SYSTEM}.dat", "/LICENSE"], check=True)


def read_dat(path):
    """{No-Intro name: [sha1 of the data, ...]}"""
    games = {}
    text = open(path, encoding="utf-8", errors="replace").read()
    for block in re.finditer(r'game \(\s*name "([^"]+)"(.*?)\n\)', text, re.S):
        # each game is listed twice: the .nes file (hashes over No-Intro's header too) and the .unh file (the
        # data alone, what ONE hashes): only the latter's sha1
        for sha in re.findall(r'name "[^"]*\.unh"[^)]*?sha1 ([0-9A-Fa-f]{40})', block.group(2)):
            games.setdefault(block.group(1), []).append(sha.upper())
    return games


def read_cht(path):
    """[(description, codes)] of the Game Genie cheats in a .cht file"""
    keys = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r'\s*(cheat\d+_(?:desc|code))\s*=\s*"?(.*?)"?\s*$', line)
        if m:
            keys[m.group(1)] = m.group(2)
    cheats = []
    n = 0
    while f"cheat{n}_code" in keys or f"cheat{n}_desc" in keys:
        codes = [c.strip().upper().replace("-", "") for c in keys.get(f"cheat{n}_code", "").split("+")]
        desc = " ".join(html.unescape(keys.get(f"cheat{n}_desc", "")).split())  # (some have &quot; and the like)
        if codes and all(GG.match(c) for c in codes):
            cheats.append((desc, "+".join(codes)))
        n += 1
    return cheats


def main():
    global OUT
    args = [a for a in sys.argv[1:] if a != "-v"]
    if "-o" in args:
        i = args.index("-o")
        OUT = args[i + 1]
        del args[i:i + 2]
    src = args[0] if args else None
    tmp = None
    if src is None:
        tmp = tempfile.TemporaryDirectory()
        src = tmp.name
        clone(src)
    commit = subprocess.run(["git", "-C", src, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    games = read_dat(os.path.join(src, "metadat", "no-intro", f"{SYSTEM}.dat"))
    cht_dir = os.path.join(src, "cht", SYSTEM)

    # Matching a cheat file's game name to No-Intro names (the files were named by hand, over the years):
    #  1. exact, once the characters RetroArch replaces in file names (& * / : ` < > ? \ |) are '_' on both sides
    #     and region lists compare in any order ("(USA, Japan)" = "(Japan, USA)");
    #  2. else by the title alone (no parenthesized parts), only if No-Intro has exactly one game with that title;
    #  3. else, for a "(Game Genie)" file, the title's one plain US release within the file's regions.
    lang = re.compile(r"^[A-Z][a-z](,[A-Z][a-z])*$")  # No-Intro language tags: (En), (En,Fr,De)

    def parts_of(name):
        return [p for p in re.findall(r"\(([^)]*)\)", name) if not lang.match(p.replace(" ", ""))]

    def key(name):
        name = re.sub(r'[&*/:`<>?\\|]', "_", name)
        parts = parts_of(name)
        title = re.sub(r"\s*\([^)]*\)", "", name).strip().lower()
        return (title, tuple(sorted(frozenset(x.strip().lower() for x in p.split(",")) for p in parts) and
                             [",".join(sorted(x.strip().lower() for x in p.split(","))) for p in parts]))
    exact = {}
    by_title = {}
    for name in games:
        exact.setdefault(key(name), []).append(name)
        by_title.setdefault(key(name)[0], []).append(name)

    def match(game, gamegenie_file):
        if game in games:
            return game, 1
        k = key(game)
        if len(exact.get(k, [])) == 1:
            return exact[k][0], 1
        if len(by_title.get(k[0], [])) == 1:
            return by_title[k[0]][0], 2
        if gamegenie_file:
            # Game Genie codes were written for the US release: the one plain release (only a region in its
            # parentheses) of that title whose regions include USA and lie within the file's ("World" counts as all)
            file_regions = set(",".join(x for x in re.findall(r"\(([^)]*)\)", game)).lower().replace(" ", "").split(","))
            us = []
            for name in by_title.get(k[0], []):
                parts = parts_of(name)
                if len(parts) != 1:
                    continue
                regions = set(parts[0].lower().replace(" ", "").split(","))
                if "usa" in regions and ("world" in file_regions or regions <= file_regions | {"world"}):
                    us.append(name)
            if len(us) == 1:
                return us[0], 3
        return None, 0

    by_game = {}  # No-Intro name -> [(desc, codes)], cheats of all its files, without repeats
    unmatched = []
    tiers = {1: 0, 2: 0, 3: 0}
    for name in sorted(os.listdir(cht_dir)):
        if not name.endswith(".cht"):
            continue
        cheats = read_cht(os.path.join(cht_dir, name))
        if not cheats:
            continue  # (no Game Genie codes in it)
        game, tier = match(SUFFIX.sub("", name[:-4]), "(game genie)" in name.lower())
        if game is None:
            unmatched.append(name)
            continue
        tiers[tier] += 1
        have = by_game.setdefault(game, [])
        for cheat in cheats:
            if cheat[1] not in [c[1] for c in have]:
                have.append(cheat)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as out:
        out.write("# ONE's Game Genie code database, an adaptation of libretro-database (CC-BY-SA-4.0)\n")
        out.write(f"# built by tools/cheats/build_db.py from libretro-database {commit}\n")
        out.write("# @<SHA-1 of the ROM data (PRG+CHR, no header)> <No-Intro name>, then <code>[+<code>]<TAB><description>\n")
        for game in sorted(by_game):
            for sha in games[game]:
                out.write(f"@{sha} {game}\n")
                for desc, codes in by_game[game]:
                    out.write(f"{codes}\t{desc}\n")
    n_cheats = sum(len(c) for c in by_game.values())
    print(f"build_db.py: {len(by_game)} games, {n_cheats} cheats -> {OUT} "
          f"({os.path.getsize(OUT) // 1024} KB); cht files matched by name: {tiers[1]}, by title alone: {tiers[2]}, Game Genie files given "
          f"the US release: {tiers[3]}, "
          f"not matched: {len(unmatched)}", file=sys.stderr)
    if "-v" in sys.argv:
        for name in unmatched:
            print("  unmatched:", name, file=sys.stderr)


if __name__ == "__main__":
    main()
