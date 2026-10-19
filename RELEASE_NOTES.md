# ONE 0.1.0

The first public release of ONE, an accurate, fast NES emulator written in C, with a SoftICE-style debugger.
The name stands for "One NES Emulator".

## What is in this release

| File | What it is |
|---|---|
| `one-libretro-windows-x64-pgo-0.1.0.zip` | the libretro core for RetroArch, 64-bit Windows (`one_libretro.dll` and `.info`) |
| `one-libretro-linux-x86_64-pgo-0.1.0.tar.gz` | the libretro core for RetroArch, Linux (`one_libretro.so` and `.info`) |
| `one-windows-x64-pgo-0.1.0.zip` | the stand-alone emulator `one.exe` for 64-bit Windows, with its menu and Lua scripting |
| `ONE-0.1.0-x86_64.AppImage` | the stand-alone emulator `one` for Linux, in one file |
| `one-debugger-windows-x64-0.1.0.zip` | the debugger `one-debugger.exe` for 64-bit Windows with the quick reference |
| `ONE-Debugger-0.1.0-x86_64.AppImage` | the debugger `one-debugger` for Linux, in one file |
| `ONE-0.1.0-source.zip` | the source code (GitHub also attaches its own "Source code" archives): see [BUILDING.md](BUILDING.md) |
| `SHA256SUMS.txt` | checksums of the files above |

The emulators and the libretro cores are profile-guided (PGO) builds; the debugger is a plain Release build. The
Windows builds are cross-built with MinGW-w64 on Linux and trained under Wine, and need nothing but the Windows system
libraries. The Linux files are built on Ubuntu 22.04 and run on any x86-64 distribution with glibc 2.35 or newer
(Debian 12, Ubuntu 22.04, Linux Mint 21, Fedora 36 and later). The AppImages carry SDL2 inside; they need the graphics
driver and the sound server of your system. To run one: `chmod +x ONE-0.1.0-x86_64.AppImage`, then
`./ONE-0.1.0-x86_64.AppImage game.nes`. Each zip and tar.gz has the license and the documents; the AppImages carry them
in `usr/share/doc/one` (`--appimage-extract` unpacks one).

## Installing the libretro core (RetroArch)

1. Copy `one_libretro.dll` (Windows) or `one_libretro.so` (Linux) into RetroArch's `cores` folder and `one_libretro.info` into its `info` folder.
2. In RetroArch: Load Core > ONE, then Load Content and pick a `.nes` file (a `.zip` with one works too).

The core has picture, sound, two joypads, the Zapper, save states (rewind and run-ahead work), reset, Game Genie
codes, battery RAM in RetroArch's save folder, and a few core options. It has no Lua scripts and no menu of its own:
RetroArch has those.

## Highlights

* **Accuracy.** Cycle-accurate CPU, PPU, APU and DMA. It passes all 144 tests of
  [AccuracyCoin](https://github.com/100thCoin/AccuracyCoin), 171 of 173 traced test ROMs (the two differences depend
  on the console's power-on warm-up), and 32 of 32 mapper test ROMs.
* **42 mappers**, covering the majority of the NES catalog: iNES numbers 0-5, 7, 9-11, 16, 19, 21-26, 32-34, 37, 47,
  66, 69-71, 75, 78, 79, 86-88, 105, 113, 118, 119, 184, 185, 206, 232 and 243, with the expansion audio of MMC5,
  VRC6, Namco 163 and Sunsoft 5B.
* **Speed.** The Windows core is profile-guided: about 22% faster than the same code built without it (382 and
  313 frames a second on one test machine, an AMD Ryzen 7 5700G, over eight games; see
  [docs/pgo.md](docs/pgo.md)). The emulation is identical: the pictures after 600 frames were the same in every build.
* **The debugger** (`one-debugger`, built from the source): registers, PPU, code, memory and OAM windows, breakpoints,
  a call stack, a code/data log, labels and comments, background, palette and pattern viewers, and a Cheat Search that
  finds a Game Genie code for you. An MCP server lets an AI agent drive it ([docs/debugger.md](docs/debugger.md)).
* **Lua scripting** with both Mesen 2's and FCEUX's APIs ([docs/lua.md](docs/lua.md)).
* **Game Genie.** Codes for a game, remembered per game. ONE does not ship a code database: build your own with
  `tools/cheats/build_db.py` (see `data/cheats/README.md`).

## Known limits

* No PAL timing: NTSC only.
* The Windows debugger's remote channel (`--remote`, for the MCP adapter) is TCP on 127.0.0.1, not a Unix socket: the
  token file is the only protection, and the adapter on Windows has been tried under Wine only.
* Mapper 16 has no barcode reader and no 157, 159 or 24C01 support (Datach Battle Rush is not yet right), and
  mapper 37 has no PAL timing. A ROM with a mapper that is not in the list above does not start.
* RAMBO-1 (mapper 64) is not included; it is being rewritten.
* A Visual Studio (MSVC) build is supported ([BUILDING.md](BUILDING.md)) but not part of this release.
* The profile-guided builds were trained on the author's own set of test ROMs and games (listed in
  `tools/pgo_games.txt`), which are not part of the repository.

## License

GNU General Public License, version 2 or (at your option) any later version: see [LICENSE](LICENSE) and
[THIRD_PARTY.md](THIRD_PARTY.md). Test ROMs and games are not included; they have their own licenses.
