# Game Genie codes

ONE offers the known Game Genie codes for the loaded game (`cheats.c`; the debugger's `gg db`; the menu's Game Genie
page), found by the game's SHA-1 (of its PRG + CHR data, without the iNES header). Two sources, the user's first:

1. `nes_gamegenie.txt` in the `cheats` folder of ONE's config directory (`~/.config/one/cheats/`,
   `$XDG_CONFIG_HOME/one/cheats/`, `%APPDATA%\one\cheats\`). ONE does not ship it: make it with

       python3 tools/cheats/build_db.py

   which clones libretro-database (<https://github.com/libretro/libretro-database>, CC BY-SA 4.0) and writes the file
   there (`-o FILE` for another place). Its license does not allow it in a GPL-2.0 program, so it stays the user's own
   copy: it is not part of ONE's repository or releases.
2. `default.txt` here, compiled into the programs: a few codes ONE's author found by hand (Ninja Gaiden), under the
   same license as ONE.

You can add games to your own `nes_gamegenie.txt` by hand in the format below.

## Format

    # comments
    @<SHA-1 of the ROM data: PRG + CHR, without the 16-byte iNES header> <name>
    <code>[+<code>...]<TAB><description>

Several codes joined by `+` are one cheat (they work together).
