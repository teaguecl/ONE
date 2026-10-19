# ONE

ONE is an accurate, fast NES emulator written in C, with a SoftICE-style debugger featuring a MCP server.  The name is a recursive acronym standing for "One NES Emulator".

* **The emulator** (`one`): cycle-accurate CPU, PPU, APU and DMA, checked against the standard
  hardware test ROMs; the Zapper and the Four Score; Game Genie codes; save states; many mappers.
* **The debugger** (`one-debugger`): optimized for new game development, reverse engineering and Game Genie code development. A quick reference is in `docs/quickref/`.  **Linux only for now.**
* **Lua scripting**: supports both Mesen 2's Lua API and FCEUX's, so scripts written for either run (`docs/lua.md`).
* **A libretro core** (`one_libretro`) for RetroArch.

## Motivation
There are dozens of NES emulators, the world does not need another one.  ONE exists because I'd like for it's debugger to be unique.  It can be used for new game development, as well as finding Game Genie codes with it's "Cheat Search" feature.  It's also effective at interactive reverse engineering - any comments or symbols added during a session will be saved.  LUA Scripting allows for highly customized tools to be developed.  Use it's MCP server and let an AI agent accelerate your RE and debugging tasks.

If you just want to play games, ONE's libretro core will give you a very accurate, reasonably fast experience with all the bells and whistles of Retroarch - but it's not significantly different than any other NES core.

## Building

The easiest way is the build container, which has every tool ONE needs. You only need Docker (or Podman, with
`DOCKER=podman`) and a shell; nothing else is installed on your machine. `tools/dev.sh` runs a command in the container:

```sh
bash tools/dev.sh cmake --preset linux-debugger          # the emulator, the debugger and the menu
bash tools/dev.sh cmake --build --preset linux-debugger -j
./build/linux-debugger/one game.nes                      # run on the host (needs SDL2 2.30 or newer)
./build/linux-debugger/debugger/one-debugger game.nes
```

The container is for building and testing; run the programs on the host. The first command also builds the
container image.

Or build directly on Debian, Ubuntu or Linux Mint:

```sh
sudo apt install build-essential cmake libsdl2-dev
cmake --preset linux-debugger
cmake --build --preset linux-debugger
```

CMake downloads Lua, Dear ImGui and (for Windows builds) SDL2 when it configures. Options, other presets, the
Windows builds and the libretro core are in [BUILDING.md](BUILDING.md); running ONE, its menu and settings
are in [docs/usage.md](docs/usage.md), and the debugger is in [docs/debugger.md](docs/debugger.md).

## Accuracy

ONE is checked against hardware test ROMs. Run `tools/fetch_test_roms.sh` to download most of the ROMs, and tests whose inputs are missing are skipped.

| Suite | Result | Notes |
|---|---|---|
| [AccuracyCoin](https://github.com/100thCoin/AccuracyCoin) | **144 / 144** | All tests pass (2026-10-18). |
| Test ROMs (blargg, nestest and others) | 171 / 173 | CPU, PPU, APU, DMA, interrupt, sprite and timing tests. The 2 differences are tests that depend on the console's power-on warm-up period (`scroll_idle`, `RasterDemo`). |
| Mapper test ROMs | 32 / 32 | |

## Mappers

ONE supports 42 mappers covering the majority of the NES catalog.  Is supports iNES numbers 0-5, 7, 9-11, 16, 19, 21-26, 32-34, 37, 47, 66, 69-71, 75, 78, 79, 86-88, 105, 113, 118, 119, 184, 185, 206, 232 and 243. Most were checked against commercial games, and the expansion audio of MMC5, VRC6, Namco 163 and Sunsoft 5B against recordings.
Known gaps: 16 has no barcode reader, 157, 159 or 24C01 (Datach Battle Rush is not yet right), and 37 has no PAL timing. RAMBO-1 (64) is being rewritten.
A ROM with any other mapper does not start: `one` prints `error: unsupported mapper: N`.

## Tests

```sh
bash tools/check.sh          # build and run the fast tests (about a minute)
```

The accuracy tests that need third-party test ROMs or games skip when those are missing. CONTRIBUTING.md has the
details.

## Future Enhancments
I'd love to hear from anyone using ONE - I'd love to improve it.  Below is a list of things that may get done in the future.

* PAL Support
* RAMBO-1 mapper (iNES 64)
* Alternate debugger UI
* VS Code integration
* Mac support

## License

ONE is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License
as published by the Free Software Foundation, either version 2 of the License, or (at your option) any later
version. See [LICENSE](LICENSE) (GPL version 2). Every source file carries an `SPDX-License-Identifier:
GPL-2.0-or-later` line.

Copyright (C) 1999-2026 Chris Teague.

ONE is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

The libraries ONE uses, and the files of other people in this repository, have their own licenses:
see [THIRD_PARTY.md](THIRD_PARTY.md).
