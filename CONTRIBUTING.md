# Contributing to ONE

Thanks for looking. ONE is a NES emulator that aims to be very accurate and very fast, so changes are judged on
both: discussion may be needed for a fix that is right but slows the emulation down, or a speedup that changes behavior.

## Build and test

```sh
cmake --preset linux-debugger && cmake --build --preset linux-debugger   # emulator, debugger, menu
bash tools/check.sh                                                      # the fast tests, about a minute
```

`tools/check.sh` has three tiers: `fast` (every change), `full` (before you ask for a review: adds the slow accuracy
tests and clang-tidy) and `nightly`. Details and every build option: [BUILDING.md](BUILDING.md).

Most of the accuracy tests compare ONE with Mesen traces of third-party test ROMs. Those are not in the repository
(the traces are gigabytes): `tools/fetch_test_roms.sh` downloads the ROMs, and those tests are skipped when a ROM or
trace is missing. A test you add must skip the same way when its inputs are not there.

## Rules

* **The core in `src/` is C.** C++ is only for `debugger/` and `gui/`; `gui/` uses the emulator through `frontend.h`
  only (`python3 tools/separation_check.py`).
* **Format and lint:** `bash tools/format.sh` (clang-format 18) before you commit, and
  `bash tools/tidy.sh -p build/linux-debugger FILES...` on the files you changed.
* **License:** ONE is GPL-2.0-or-later. By sending a change you agree to license it that way. Every source file starts
  with the notice; `python3 tools/license_headers.py` adds it to new files (`--check` lists the ones that lack it).
* **No games, no copied code.** Do not add ROMs, save states or traces made from commercial games. Do not copy code from
  other emulators; describe hardware from the nesdev wiki, hardware tests and your own measurements.
* **New `static` variables in the core** must be listed in `STATE()` or `STATE_SKIP()` (the `savestate_fields` test
  fails otherwise), so save states stay complete.
* **Tests:** a behavior change comes with a test (a `--batch` script in `tests/`, or a ROM and trace). A test that takes
  more than about ten seconds gets the `slow` label (`tests/CMakeLists.txt`).

## Reporting accuracy bugs

Say which ROM, which mapper, what you expected and what ONE does, and give the evidence: a hardware test ROM, a
nesdev page, or a trace from another emulator that you trust. Please do not attach game ROMs.

## Layout

| Directory | What |
|---|---|
| `src/`, `src/mappers/` | the emulator core (C) and the mapper boards |
| `debugger/` | `one-debugger`, the SoftICE-style debugger (C++, Dear ImGui) |
| `gui/` | the in-game menu (C++, Dear ImGui) |
| `libretro/` | the libretro core |
| `lua/`, `docs/lua.md` | the Mesen 2 / FCEUX Lua layer |
| `tests/`, `regression/` | tests, the Mesen trace regression |
| `tools/` | check, format, tidy, test ROM fetching, profiling, MCP server |
| `third_party/` | libco, getopt, license texts ([THIRD_PARTY.md](THIRD_PARTY.md)) |

## Tests in detail

```sh
ctest --test-dir build -L smoke --output-on-failure     # or: ctest --preset linux-release
```

* `one_help`, `nestest`: quick smoke tests (nestest is compared line by line with a Mesen trace).
* `debugger_batch` (when the debugger is built): runs `tests/debugger_smoke.txt` through `one-debugger --batch`.
* `debugger_config_*`: both config files in `debugger/` must load without warnings (keeps them in step with the code).
* `debugger_banked_labels`: bank-exact labels in a small MMC1 test ROM (`tests/banked/`).
* `debugger_search_stretch` (needs Python 3): the memory search's filters typed one after another judge the same
  stretch of play, in either order (`cs same` + `cs notwritten`), and `cs undo` gives the stretch back
  (`tests/search_check.py`).
* `savestate_roundtrip_nestest`, `_mmc1`, `_cnrom` (smoke), `_dmc`, `_apu_test` (label `savestate`, need
  Python 3): a state saved during a run, loaded into a fresh `one`, must give the same CPU trace and APU log as the
  original run, and the same state 20 frames later, byte for byte (`tests/state_roundtrip.py`). At least one ROM
  per mapper: `tests/mmc1/` and `tests/cnrom/` are small ROMs (ca65 source included) that keep every mapper
  register busy, saved at frames chosen so that leaving any mapper variable out of the state fails the test.
  A new mapper needs one too. The one-latch mappers (UxROM, AxROM, Color Dreams, BNROM)
  have `--batch` tests instead (`debugger_mapper_*`; `tests/discrete/gen.py` makes their ROMs and scripts), as does the MMC5
  (`debugger_mapper_exrom`, `tests/exrom/gen.py`; its round trip is `savestate_roundtrip_exrom`).
* `console_output` (labels `smoke` and `lint`, needs Python 3): the core writes to the console only through
  `log.h` (`tools/print_check.py`).
* `savestate_fields` (label `lint`, needs clang 18): every variable of the core is saved or marked as not machine
  state (`tools/state_check.py`).
* `debugger_screenshot`, `debugger_screenshot_png` (need Python 3): `screenshot` in batch mode writes a valid
  256x240 PNG of nestest's menu (`tests/png_check.py`).
* `input_gamepads` (SDL2 builds): controller bindings with SDL virtual gamepads, and the hotkeys with synthetic
  key presses (`tests/input_test.c`).
* `settings_table`: the settings table: defaults, reading a file with mistakes in it, refused values, saving
  changes in place with the comments kept, creating the user's file (`tests/settings_test.c`).
* `gui_menu` (builds with the menu): the menu on SDL's dummy video driver, driven by injected key presses and a
  virtual gamepad: every page, changing settings of each kind, choosing keys and gamepad buttons, saving into
  the settings file, and in a running nestest: opening the menu with Esc, saving to a slot and loading it again
  (`gui/gui_test.cpp`; `GUI_TEST_SHOTS=DIR` also writes a picture of each step).
* `gui_separation` (labels `smoke` and `lint`, needs Python 3): the menu uses the emulator through `frontend.h`
  only (`tools/separation_check.py`).
* `clang_format` (labels `smoke` and `lint`): every source file must be formatted (see below); skipped when
  clang-format 18 is not installed.
* `clang_tidy` (label `lint` only: it takes about a minute): no clang-tidy findings (see below); skipped when
  clang-tidy 18 is not installed. `ctest --test-dir build -L lint` runs both checks.
* `audio_mixer_square`, `_triangle`, `_dmc` (label `audio`, about 15 s, need Python 3 and
  `test_roms/apu_mixer/`): blargg's mixer ROMs must play near silence between their beeps
  (`tools/audio_check.py`), and where `test_roms/apu_reference/apu_<rom>.txt` exists, ONE's APU log must match Mesen's
  (`tools/apu_compare.py`). `ctest --test-dir build -L audio`.
* `regression_quick` (label `regression`, needs bash): runs `regression/regtest.sh quick` against every Mesen log
  in `regression/tests.txt`. Tests whose ROM or log is not present are reported as SKIPPED, so the large logs do
  not have to be committed. Run it directly with
  `ONE=build/linux-release/one regression/regtest.sh quick|full [filter]`.

## Code formatting

The style is in `.clang-format` (4-space indents, braces on the line of their `if`/`for`/`struct`, a function's
brace on its own line, 120 columns) and is defined by **clang-format 18**: other versions format slightly
differently, so `tools/format.sh` insists on 18 (`sudo apt install clang-format-18`; `CLANG_FORMAT=...` picks
another binary).

```sh
tools/format.sh             # reformat every source file    (or: cmake --build build --target format)
tools/format.sh --check     # list unformatted files, fail  (or: --target format-check)
tools/format.sh FILE...     # only these files
```

ONE's own sources are formatted: `*.c`/`*.h` at the top, `debugger/*.cpp`/`*.hpp` and `tests/*.c`; third-party
`third_party/` is left alone. Hand-aligned tables that must keep their layout are wrapped in
`// clang-format off` ... `// clang-format on`.

## Static analysis (clang-tidy)

`.clang-tidy` (and `debugger/.clang-tidy`, `gui/.clang-tidy` for the C++ parts) picks checks that find bugs rather
than enforce a style: the clang static analyzer, `bugprone-*`, `performance-*`, `misc-*`, `portability-*`, a few `readability-*`,
and for the debugger some `modernize-*`/`cppcoreguidelines-*`. Magic numbers, globals and deliberate narrowing to
8/16 bits are normal in an emulator, so those checks are off. Every finding is an error. Needs **clang-tidy 18**
(`sudo apt install clang-tidy-18`) and a configured build (it uses that build's `compile_commands.json`).

```sh
tools/tidy.sh                  # check everything (about a minute)  (or: cmake --build build --target tidy)
tools/tidy.sh FILE...          # only these files; a header checks the files that include it
tools/tidy.sh -p DIR --fix     # use build directory DIR; apply clang-tidy's fixes
cmake -DONE_CLANG_TIDY=ON ...  # check every file while it is compiled (findings fail the build; CMake 3.27+)
```

The core is compiled with and without the debugger hooks, and both versions are checked. A finding that is
deliberate gets `// NOLINT(check-name): reason` on its line.

## Pre-commit hook

To have every commit checked, install the pre-commit hook once (after `git init` or `git clone`):

```sh
ln -s ../../tools/pre-commit .git/hooks/pre-commit
git config blame.ignoreRevsFile .git-blame-ignore-revs   # git blame skips commits that only reformat
```

The hook refuses the commit if a changed source file is not formatted (it checks the staged version;
`tools/format.sh FILE && git add FILE` fixes it) or has clang-tidy findings (a few seconds per file;
`ONE_SKIP_TIDY=1 git commit ...` skips just that). `git commit --no-verify` commits anyway.

## Continuous integration

`.github/workflows/ci.yml` checks the formatting (clang-format 18) and builds and runs the smoke tests on every
push and pull request:
Linux GCC and Clang (Release), Linux GCC Debug with sanitizers (all three also build and test `one-debugger`),
Windows MSVC and Windows MSYS2/MinGW (emulator only).
The Linux and MSVC jobs upload packaged binaries (`cpack`) as workflow artifacts.

## Releases and the version number

The version is `MAJOR.MINOR.PATCH` (`0.x` until ONE is called stable), set in one place: `project(one VERSION ...)` in
`CMakeLists.txt`. `one --version`, `one-debugger --version` (and its `version` command), the menu, the libretro core
and Lua's `one.version()` all report it. A build of exactly the commit tagged `v<version>` from a clean tree says
`0.1.0`; any other build adds the commit, `0.1.0+g1a2b3c4` (`-dirty` if tracked files have local changes), so a bug
report says which code it ran (`cmake/version.cmake` makes `one_version.h` at every build).

To make a release:

1. Change the version in `CMakeLists.txt` and in `libretro/one_libretro.info` (`display_version`); the ctest
   `version_consistency` fails if the programs, the project and that file disagree.
2. Add the release to `CHANGELOG.md`.
3. Commit, then tag that commit `v<version>` (for example `v0.1.0`). Build from the tagged commit, clean.

The save-state file has a layout number of its own (`FORMAT_VERSION` in `src/savestate.c`); it changes only when the
layout does, not with each release.
