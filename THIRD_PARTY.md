# Third-party software and data

ONE itself is GPL-2.0-or-later (see [LICENSE](LICENSE)). Everything that is compiled into ONE is under a license
compatible with that. The license texts of the libraries CMake downloads are in `third_party/licenses/`: include them
with any binary you hand out.

## In this repository

| Path | What | License |
|---|---|---|
| `third_party/libco/` | [libco](https://github.com/higan-emu/libco) v20 by byuu, the coroutine library behind the CPU / PPU switches (`third_party/libco/README.ONE`) | ISC (`third_party/libco/LICENSE`); `third_party/libco/valgrind.h` has its own BSD-style notice |
| `libretro/libretro.h` | the libretro API header, RetroArch team | MIT (the statement in the file) |
| `tests/data/nestest.nes`, `nestest_mesen.txt` | nestest, Kevin Horton's CPU test ROM (freely distributed), and a Mesen trace of it | the ROM is its author's, free to redistribute; the trace is output of Mesen on it |
| `data/cheats/default.txt` | the Game Genie codes built in, found by ONE's author | GPL-2.0-or-later, like ONE |
| `src/zipfile.c` | its small inflate is written after zlib's `contrib/puff` | puff is public domain; the code is ONE's |

## Downloaded when you build

CMake fetches these; they are not in the repository and not changed.

| Library | Version | License |
|---|---|---|
| [Lua](https://github.com/lua/lua) | 5.4.7 | MIT (`third_party/licenses/lua-MIT.txt`) |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.9b | MIT (`third_party/licenses/imgui-MIT.txt`) |
| [SDL2](https://libsdl.org) (Windows builds and the Linux AppImages, linked in; other Linux builds use the system's) | 2.30.9 | zlib (`third_party/licenses/SDL2-zlib.txt`) |

## Not included, and why

* **Game Genie code database.** [libretro-database](https://github.com/libretro/libretro-database) is CC BY-SA 4.0,
  which the GPL version 2 does not allow in ONE, so ONE neither includes nor embeds it. `tools/cheats/build_db.py`
  builds a copy for your own use in `~/.config/one/cheats/`, which ONE reads when it is there (`data/cheats/README.md`).
  Game names and hashes in it come from [No-Intro](https://www.no-intro.org).
* **Test ROMs** (blargg's, AccuracyCoin, and others) are not in this repository. They have their own licenses and
  their authors' names; `tools/fetch_test_roms.sh` downloads nes-test-roms into `test_roms/`.
* **Games and Mesen / FCEUX traces made from them** are never part of the repository.

## Compatibility with other emulators

ONE implements [Mesen 2's Lua API](https://www.mesen.ca/docs/apireference.html) and FCEUX's Lua functions, and reads
Mesen's `.mlb` labels and CDL files, and ld65 debug files, so their scripts and files work with it. The Mesen traces
the regression tests compare with are made by running Mesen on a game and are not distributed.
