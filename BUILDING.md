# Building ONE

ONE is built with [CMake](https://cmake.org) (3.21 or newer) and uses
[SDL2](https://libsdl.org) for its window and keyboard input. SDL2 is optional: a *headless* build (no window)
is what the trace tests use.

## What you get

ONE builds three programs from one emulator core:

* **`one-debugger`**: a SoftICE-style debugger for NES programs. Registers, code, memory, PPU, OAM, palette and
  watch windows, breakpoints, labels, tracing and a command language that also runs in `--batch` mode. It runs the
  game in its own window beside the debugger.
* **`one`**: the emulator. A game window (SDL2) with an in-game menu (Esc) for settings, controllers, save states,
  Lua scripts and Game Genie codes. It is a plain, fast way to play and to run scripts.
* **`one_libretro`**: ONE as a libretro core (`one_libretro.so` on Linux, `.dll` on Windows) for RetroArch or another
  libretro frontend. It has no window or menu of its own, but the frontend provides a fuller playing experience than
  `one` does: rewind and run-ahead, on top of save states, Game Genie codes and
  battery RAM, plus whatever else the frontend offers.

## Recommended: build in a container (Docker)

The easiest and most reproducible way to build ONE is in a container that has every tool needed, so there is nothing
to install on your machine except Docker (or Podman) and a shell
(`docker/Dockerfile`: Ubuntu 24.04 with GCC 13, Clang 18, CMake, SDL2, Python, clang-format/clang-tidy 18,
cc65, MinGW-w64, pandoc and Chrome). `tools/dev.sh` runs one command in it:

```sh
bash tools/dev.sh cmake --preset linux-debugger
bash tools/dev.sh cmake --build --preset linux-debugger -j
bash tools/dev.sh ctest --preset linux-debugger
bash tools/dev.sh bash tools/format.sh --check
bash tools/dev.sh                         # a shell in the container
./build/linux-debugger/debugger/one-debugger game.nes   # then run it on the host
```

* Every command runs in a new container that is deleted afterwards (`docker run --rm`); nothing is kept in it. The
  working tree is mounted at the same path as on the host, and everything is written there (build directories,
  CMake's downloads of Dear ImGui / SDL2) as the calling user.
* The image is built on first use and rebuilt automatically when `docker/Dockerfile` changes (its tag is the file's
  hash). `DOCKER=podman` uses Podman instead.
* The container is for building and testing: run `one` and `one-debugger` on the host (a build made in the
  container runs on Ubuntu 24.04 / Linux Mint 22 and other systems with SDL2 2.30 or newer). Don't mix host and
  container builds in one build directory; CMake keeps the compiler it found first.
* Chrome (for the quick reference) is installed on x86-64 only; `ONE_QUICKREF=0 bash tools/dev.sh ...` makes an
  image without it.

## Building directly on Linux

If you prefer not to use the container:

```sh
sudo apt install build-essential cmake libsdl2-dev     # Debian, Ubuntu, Linux Mint
cmake --preset linux-release
cmake --build --preset linux-release
./build/linux-release/one game.nes      # or game.zip (its first .nes file)
```

The other presets are listed in the table below.

Without presets:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

`make` in the repository root does the same (it is a thin wrapper around CMake).

## CMake presets

Each preset configures into `build/<preset name>`. Use `cmake --preset NAME`, then `cmake --build --preset NAME`;
presets marked "yes" under Tests also work with `ctest --preset NAME` (the smoke tests).

| Preset | Platform | Builds | Description | Tests |
|---|---|---|---|---|
| `linux-release` | Linux | `one` | Release build with the in-game menu. The one to play with. | yes |
| `linux-debugger` | Linux | `one`, `one-debugger` | RelWithDebInfo build that also builds the debugger (Dear ImGui). | yes |
| `linux-debug` | Linux | `one` | Debug build with AddressSanitizer/UBSan and warnings as errors. | yes |
| `headless` | Any | `one` (no window) | Release build without SDL2, for tests and traces. | yes |
| `libretro` | Linux | `one_libretro.so` | The libretro core only: no SDL2, no menu. | no |
| `windows-msvc` | Windows | `one.exe` | Visual Studio 2019 or newer; SDL2 is downloaded and linked statically. | yes |
| `windows-mingw` | Windows | `one.exe` | MinGW-w64 in an MSYS2 UCRT64 shell; SDL2 from pacman. | yes |
| `windows-cross` | Linux | `one.exe` | Cross-compiles for Windows with MinGW-w64; SDL2 is downloaded. | no |
| `windows-msvc-libretro` | Windows | `one_libretro.dll` | The libretro core with Visual Studio. | no |
| `windows-msvc-debugger` | Windows | `one.exe`, `one-debugger.exe` | As `windows-msvc`, plus the debugger. Not tried natively yet. | no |
| `windows-mingw-debugger` | Windows | `one.exe`, `one-debugger.exe` | As `windows-mingw`, plus the debugger. Not tried natively yet. | no |
| `windows-cross-debugger` | Linux | `one.exe`, `one-debugger.exe` | As `windows-cross`, plus the debugger; `ctest --preset windows-cross-debugger` runs the smoke tests under Wine. | no |
| `windows-cross-libretro` | Linux | `one_libretro.dll` | The libretro core for Windows, cross-compiled with MinGW-w64. | no |

## The libretro core (RetroArch)

ONE also builds as a libretro core: `one_libretro.so` on Linux (below) and `one_libretro.dll` on Windows (presets
at the end of this section):

```sh
cmake -S . -B build/libretro -DCMAKE_BUILD_TYPE=Release -DONE_BUILD_LIBRETRO=ON
cmake --build build/libretro --target one_libretro
retroarch -L build/libretro/one_libretro.so game.nes
```

It has picture, sound, two joypads, save states (rewind and run-ahead work), reset, Game Genie codes, battery RAM
in RetroArch's save directory and a few core options (aspect ratio, overscan, treble cut). No Lua scripts and no
menu of its own: RetroArch has those. Its messages go to RetroArch's log. Copy `libretro/one_libretro.info` next to RetroArch's other core info files
to give it a proper name there. Presets: `libretro` (this, with no SDL2 or menu), and for Windows
`windows-cross-libretro` (MinGW-w64 from Linux) or `windows-msvc-libretro` (Visual Studio), which build
`one_libretro.dll` (`cmake --preset ... && cmake --build --preset ...`; in the container:
`bash tools/dev.sh cmake --preset windows-cross-libretro`, then `bash tools/dev.sh cmake --build --preset windows-cross-libretro`). `ctest -R libretro_core` runs a small test frontend against it.

## Maximum-speed builds: profile-guided optimization (slow to build)

`tools/pgo.sh` builds `one` with profile-guided optimization (GCC, or Clang with `libclang-rt-18-dev` and
`llvm-profdata`): an instrumented build runs some ROMs headless to record where the time goes, then everything is
rebuilt with that profile, with link-time optimization (LTO) as well. The emulation is the same (the compiler only
arranges the code better). Measured with `tools/perf/bench.sh` (2026-10-08): PGO + LTO runs 7-10% faster than a
plain Release build (PGO alone 2-7%; LTO alone nothing at `-O3`, so the presets leave it off).
`-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF` after the build directory turns LTO off.

This is a script rather than a CMake preset because a PGO build is three steps in order (build an instrumented
`one`, run it on training ROMs, rebuild with the recorded profile), and a preset can only describe a single configure
and build; it cannot run the training step in between.

```sh
bash tools/pgo.sh                                   # -> build/pgo/one
bash tools/pgo.sh build/pgo -DONE_BUILD_DEBUGGER=ON # another build directory, extra CMake options
bash tools/dev.sh bash tools/pgo.sh                 # the same in the build container
bash tools/pgo.sh --libretro                        # the libretro core: build/pgo-libretro/one_libretro.so
```

* Training ROMs: by default the test ROMs in the tree (nestest, `tests/mmc1`, `tests/cnrom`, the APU tests), plus
  `test_roms/AccuracyCoin/AccuracyCoin.nes` and every `commercial_games/*.nes` if present. Games are the best training (their
  attract modes scroll, draw sprites and play music); ROMs whose mapper ONE does not support yet are skipped.
  `PGO_ROMS="a.nes b.nes"` trains on other ROMs instead (names without spaces), `PGO_SECONDS` sets the emulated
  time per ROM (default 20). A run takes a couple of minutes.
* Only `one` (or, with `--libretro`, the libretro core) is profiled; `one-debugger` is built normally in the same directory.
  Windows (MSYS2, Visual Studio, cross-built and trained under Wine) and how to compare the builds: [docs/pgo.md](docs/pgo.md).
* The two steps are CMake's `ONE_PGO=generate` and `ONE_PGO=use` (profile in `ONE_PGO_DIR`, default
  `<build>/pgo-profile`), in the same build directory. Rerun the script after changing the emulator: an older profile
  still builds, with a warning, but fits the new code less well.

## Windows

### Visual Studio (MSVC)

Open the folder in Visual Studio ("Open a local folder") and pick the `windows-msvc` preset, or from a
Developer PowerShell:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc
.\build\windows-msvc\Release\one.exe game.nes
```

The presets name no generator, so CMake uses the newest Visual Studio it finds (with the "Desktop development with C++"
workload). To pick another, add `-G "Visual Studio 17 2022"` (after deleting the build folder).

This preset downloads and builds SDL2 automatically (`ONE_FETCH_SDL2=ON`) and links it statically, so `one.exe`
needs no DLLs. To use an installed SDL2 instead (e.g. from vcpkg: `vcpkg install sdl2`), configure with
`-DONE_FETCH_SDL2=OFF -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`.

### MSYS2 / MinGW-w64

In an MSYS2 UCRT64 shell:

```sh
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-SDL2
cmake --preset windows-mingw
cmake --build --preset windows-mingw
```

### Cross-compiling from Linux

```sh
sudo apt install mingw-w64
cmake --preset windows-cross        # toolchain file: cmake/toolchains/mingw-w64-x86_64.cmake
cmake --build --preset windows-cross
```

### The debugger on Windows

`one-debugger.exe` builds with the `windows-*-debugger` presets (`bash tools/dev.sh cmake --preset
windows-cross-debugger`, then `--build`). It needs only the Windows system DLLs. It is the same program as on Linux. Settings and
window places are in `%APPDATA%\one\`, like the emulator's. `--remote` (the MCP adapter, `tools/mcp/`) works too, with TCP
on 127.0.0.1 where Linux uses a Unix socket: the token and a `PID.port` file (where it listens) are in
`%LOCALAPPDATA%\one-debugger\`, and the token is what lets a client in (a TCP peer cannot be checked as a Unix socket's
is). `ctest --preset windows-cross-debugger` runs the smoke tests that start the exe directly under Wine, and
`debugger_remote_wine` (`tests/remote_check.py --wine`, the remote channel's checks over TCP); the other tests that start it
from a script or Python are left out of that preset. The native Visual Studio and MSYS2 builds have not been tried.

### Windows release build of the libretro core

The release `one_libretro.dll` is cross-built on Linux with MinGW-w64 and profile-guided optimization, trained under
Wine (all of it is in the build container: `docker/Dockerfile` has MinGW-w64 and Wine):

```sh
ONE_QUICKREF=0 bash tools/dev.sh bash tools/pgo.sh --libretro --windows   # -> build/pgo-libretro-windows/one_libretro.dll
```

It builds twice and trains on 47 ROMs (about 25 minutes; `PGO_ROMS` and `PGO_SECONDS` shorten it). The training ROMs
are not in the repository: put test ROMs in `test_roms/` (`tools/fetch_test_roms.sh`) and your own games in
`commercial_games/`. On a Windows 11 machine (Ryzen 7 5700G) this DLL ran 22% faster than the plain cross build and
about as fast as an MSYS2 build trained on Windows; the numbers, and how to train and compare builds on Windows with
MSYS2 or MSVC, are in [docs/pgo.md](docs/pgo.md). Test the result with `libretro_host.exe`
(`build/pgo-libretro-windows/tests/`, run on Windows or under Wine): `libretro_host one_libretro.dll tests\data\nestest.nes`
must print `libretro_host: all passed`.

### Making a release

`tools/release.sh VERSION` builds every file of a release in the build containers and writes them, with
`SHA256SUMS.txt`, to `build/dist/`:

| File | Contents |
|---|---|
| `one-libretro-windows-x64-pgo-VERSION.zip` | `one_libretro.dll`, cross-built, profile-guided, trained under Wine |
| `one-libretro-linux-x86_64-pgo-VERSION.tar.gz` | `one_libretro.so`, profile-guided |
| `one-windows-x64-pgo-VERSION.zip` | `one.exe`, cross-built, profile-guided, trained under Wine |
| `ONE-VERSION-x86_64.AppImage` | `one` for Linux, profile-guided, in one file |
| `ONE-Debugger-VERSION-x86_64.AppImage` | `one-debugger` for Linux, in one file (with the quick reference inside) |
| `ONE-VERSION-source.zip` | the source, from the tag `vVERSION` |

The zips and the tar.gz also have LICENSE, README.md, THIRD_PARTY.md and the documents; the AppImages carry them in
`usr/share/doc/one`. `one-debugger-windows-x64-VERSION.zip` has `one-debugger.exe` (a plain cross build) and the quick reference. The Linux files are built on
Ubuntu 22.04 (`docker/Dockerfile.appimage`, glibc 2.35), with SDL2 and libstdc++ linked in, so they run on any x86-64
distribution with glibc 2.35 or newer (Debian 12, Ubuntu 22.04, Linux Mint 21, Fedora 36 and later). `tools/test_appimage.sh FILE.AppImage` tries
an AppImage on several distributions, each in a container.

The profile-guided builds are trained on the games listed in `tools/pgo_games.txt`. They are not in the repository:
put them into `commercial_games/` (or any directory, named in `ONE_GAMES_DIR`). The script stops before it builds
anything if one is missing. `--only NAME,...` builds some of the archives, `--quick` trains on nestest for 2 seconds
and needs no games (to try the script; do not release the result), and `--upload` makes a draft release on GitHub with
`gh` from `RELEASE_NOTES.md`. The whole run takes a few hours.

`.github/workflows/release.yml` does the same when you push a tag (`v0.1.0`), on a self-hosted runner of yours: GitHub's
own machines do not have the games. The workflow's comments say how to set it up.

## Build options

| Option | Default | Meaning |
|---|---|---|
| `ONE_ENABLE_SDL2` | ON | Window and keyboard via SDL2. OFF builds a headless-only emulator. |
| `ONE_FETCH_SDL2` | OFF (ON in the Windows presets) | Download and build SDL2 instead of using an installed one. |
| `ONE_MESEN_COMPATIBILITY` | ON | Emulate undocumented Mesen behavior (needed for byte-identical traces). |
| `ONE_ENABLE_SANITIZERS` | OFF | AddressSanitizer (+UBSan on GCC/Clang). |
| `ONE_WARNINGS_AS_ERRORS` | OFF | `-Werror` / `/WX`. CI turns this on for GCC, Clang and MinGW. |
| `ONE_BUILD_GUI` | ON | The menu of the game window (`gui/`, C++, Dear ImGui; needs `ONE_ENABLE_SDL2`). Dear ImGui is downloaded and built with ONE (`ONE_IMGUI_GIT_TAG`, default v1.92.9b). |
| `ONE_BUILD_DEBUGGER` | OFF (ON in `linux-debugger`) | Build `one-debugger` (C++). |
| `ONE_CLANG_TIDY` | OFF | Run clang-tidy on ONE's sources while compiling them (see [CONTRIBUTING.md](CONTRIBUTING.md)). |
| `ONE_PPU_CATCHUP` | ON | Let the CPU run ahead of the PPU in `one` and the libretro core (about 1.6 times faster, same results). `ONE_PPU_CATCHUP_DEBUGGER=ON` does the same in `one-debugger` (see [docs/usage.md](docs/usage.md)). |
| `ONE_LUA_GIT_TAG` | v5.4.7 | The Lua version that is downloaded and built with ONE. |
| `BUILD_TESTING` | ON | Register the CTest tests. |

## Running the tests

```sh
bash tools/check.sh                                     # build and run the fast tests, about a minute
ctest --test-dir build -L smoke --output-on-failure     # or: ctest --preset linux-release
```

What each test checks, the test tiers, code formatting, static analysis and the pre-commit hook are in
[CONTRIBUTING.md](CONTRIBUTING.md).

## Next

* Running `one`, the menu, settings, controllers, sound, hotkeys and save states: [docs/usage.md](docs/usage.md).
* The debugger: [docs/debugger.md](docs/debugger.md).
* Lua scripting: [docs/lua.md](docs/lua.md).
