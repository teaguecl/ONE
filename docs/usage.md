# Using ONE

How to run `one`, its in-game menu, settings, controllers, sound, hotkeys and save states. For the debugger see
[debugger.md](debugger.md); to build ONE see [BUILDING.md](../BUILDING.md).

## Command line

```
one [OPTIONS] [FILE]    (without FILE: the menu opens, to choose a game)
  -V, --version         print the version (`0.1.0`; a build that is not a tagged release adds the commit, `0.1.0+g1a2b3c4`) and exit
  -H, --headless        no window (always the case in builds without SDL2)
  -t, --trace           write the CPU trace (Mesen format) to stderr
      --trace-file FILE write the trace to FILE instead
  -v, --verbose         more messages (--log-level debug; see "Messages")
  -u, --unthrottled     run as fast as possible instead of at NES speed (60.1 frames/s)
  -k, --clock N         stop after CPU cycle N (default: run until the window is closed)
      --wav FILE        also write the sound to FILE (works headless too)
      --cheat CODE      a Game Genie code (6 or 8 letters, e.g. SXIOPO); may be repeated
  ...                   see `one --help`
```

### Messages

Everything ONE says goes through one small logging module (`log.h`), apart from the Mesen-format trace, which has
a stream of its own, and output asked for on the command line (`--help`, `--list-gamepads`, the `-D` dump).
Every message has a level (error, warn, info, debug, trace) and a category (`emu cpu ppu apu dma mapper input
video audio savestate cfg script`), and each category shows messages up to its own level, `info` by default:

```
one --log-level debug game.nes              # everything up to debug (-v is the same)
one --log-level info,ppu=trace,dma=trace game.nes
one --log messages.txt game.nes             # to a file instead of stderr
```

or in `one.cfg`: `log.level = info`, `log.ppu = trace` ... (the command line wins). Debug and trace messages start
with the emulation time, `[f=FRAME c=CPU_CYCLE SCANLINE,DOT]`, so they line up with a trace:

```
[f=4 c=120720 14,21] dma: DMC DMA read 00 (17 bytes left)
[f=1 c=57188 241,44] ppu: PPUCTRL ($2000) = 00
```

Messages go to stderr, except while the trace (`-t`) is written there: then they go to stdout, so that stderr holds
nothing but the trace (`--trace-file FILE` keeps them apart in any case). A message that is switched off costs one
comparison, and its arguments are not evaluated; building with `-DONE_LOG_MAX_LEVEL=LOG_LEVEL_INFO` removes debug
and trace messages from the program altogether. In the debugger, warnings and errors also appear in the command
window (which is otherwise for the debugger's own output), everything goes to its log file, and the `log` command
changes the levels while it runs (`log ppu debug`, `log` alone lists them). Keys pressed in the game window (save
states, screenshot, FPS counter) answer with a short message on the game window itself. `tools/print_check.py`
(ctest `console_output`) keeps `printf` out of the core.

### The menu (Esc)

Esc in the game window opens the menu over the paused game (sound paused too); Esc again, or Resume, goes back.
On a gamepad the guide (logo) button opens and closes it (`menu.gamepad_button`: or Back+Start, or a stick
press). It works with the mouse, the keyboard (arrows, Space to choose, Esc) or a gamepad (d-pad, A to choose,
B to go back: from a page to the list on the left, from there to the game):

* **Game**: Resume, Reset, Power cycle, Open a game... (a list of the directory's sub-directories and `.nes`
  files), Screenshot, the FPS counter, the save state slots with the time each was saved: Save / Load, and the
  10 most recently opened games (also on the Start page before a game runs; kept in
  `~/.local/state/one/recent.txt`, `$XDG_STATE_HOME`, `%APPDATA%\one\recent.txt` on Windows; only games started
  with the window and the menu are added, so headless runs leave the list alone).
* **Lua scripts** (while a game runs): Load a script... (a list of `.lua` files), the running scripts with
  Restart (after editing one, say) and Stop, Stop all, the last error a script had, and the 10 most recently run
  scripts (`~/.local/state/one/recent_scripts.txt`). Scripts keep running when another game is opened; stopping
  one takes its drawings off the picture. (`one --lua` starts them from the command line.)
* **Controllers, Hotkeys, Video, Sound, Logging**: every setting of `one.cfg`. A controller button or hotkey
  shows its inputs; choosing it asks for a key, or a gamepad button or stick (hotkeys: a key with
  Shift/Ctrl/Alt if wanted) that replaces them; `+` adds one, Clear and Default do what they say. Numbers are
  sliders (Space or A, then left/right), lists open with Space or A. A change takes effect at once, except those
  marked `*`, which need a restart (the sound device's sample rate, sound on/off).
* **Game Genie** (while a game runs): Game Genie on/off for this game; the game as the code database knows it;
  your codes, each ticked or not, with Delete; Add a code (letters only, shown decoded as you type: `$8E48 = 73 if
  it reads 8D`) and a description; the known codes for the game (your own database or the few built in: `data/cheats/README.md`), with a
  filter box: ticking one puts its codes in your list, ticked, which also switches Game Genie on. Everything is
  saved at once, per game (`~/.config/one/cheats/`), and comes back when the game is loaded; a game loaded with
  codes on says so at the bottom of the picture ("Game Genie: 2 codes").
* **Quit**.

Keys choose without moving the focus there, with or without Alt (their letter is underlined): Q quits, O opens a
game, R resumes, E resets, P power cycles, G the Game page (T the Start page), L the Lua scripts page, N the Game Genie page, and each
settings page a letter of its name (C, H, M, V, S, A); 1-9 and 0 start a recent game (on the Lua page: a recent
script). Not while a text is being edited or a key is being
chosen for a setting.

When the menu closes, the changed settings are written into the settings file ONE read, in place: comments, order
and the other lines stay as they are, and a setting the file does not have yet is added at its end. If ONE read
no file, it writes `~/.config/one/one.cfg` (`$XDG_CONFIG_HOME/one/one.cfg`; `%APPDATA%\one\one.cfg` on Windows)
with every setting documented. Note that `./one.cfg` comes first: a `one.cfg` in the directory ONE is run from
is used (and so saved into) before the one in `~/.config`.

When a game starts, "Esc: menu" appears at the bottom of the picture for 4 seconds, to say that the menu is there
(there is no menu bar to show it; the key named is `hotkey.menu`'s). `menu.hint = off` (also on the menu's Menu
page) turns it off; all of it is in `menu_hint.c`, called from `emu.c` and listed in `settings.c`.

`one` started without a ROM opens the menu to choose one. The menu is part of `one` only, not of `one-debugger`
(whose game window has no menu: Esc does nothing there).

How it is built: `gui/` is C++17 with [Dear ImGui](https://github.com/ocornut/imgui) (its SDL2 and SDL_Renderer
back ends, so SDL 2.0.18 or newer). It uses the emulator through one C header, `frontend.h`, and the settings
table, `settings.h`, and nothing else; the emulator includes nothing of it except `gui/gui.h` in `one.c`.
`tools/separation_check.py` (ctest `gui_separation`) keeps it that way. The menu runs inside the emulation's
frame loop (`input_poll()`, once a frame), which waits for it; it makes a Dear ImGui context when it opens and
destroys it when it closes.

### Settings (`settings.h`)

Every setting is described once, in a table (key, type, default, range or choices, label, description, whether it
needs a restart), each module its own (`input.c`, `video.c`, `audio.c`, `log.c`). That table reads `one.cfg`,
writes it (`one --write-config FILE`, with the descriptions as comments), builds the menu's pages, and saves the
menu's changes. A new setting is one table entry plus the function that puts its value into effect.

### Controllers and settings (`one.cfg`)

By default controller 1 is the keyboard (arrow keys, `.` = A, `,` = B, `[` = Select, `]` = Start) and the first
gamepad; controller 2 is the second gamepad. On a gamepad the d-pad and the left stick steer, the right face
button is A, the bottom one B, Back/Select is Select and Start is Start. Pads can be plugged in and out while ONE
runs, and work even while another window (the debugger's terminal) has the focus.

All of this is set in a plain-text `key = value` file: `--config FILE`, else `./one.cfg`, else
`~/.config/one/one.cfg` (`$XDG_CONFIG_HOME/one/one.cfg`). `one-debugger` reads the same file (`--one-config FILE`).
`one --write-config FILE` writes a file that lists every setting with its default and explains the syntax:

```
joy1.a = key:period, pad1:b         # any of these presses A on controller 1
joy1.up = key:up, pad1:dpup, pad1:lefty-
joy2.start = pad2:start
input.axis_threshold = 50           # percent a stick must be pushed
input.opposite_directions = off     # left+right / up+down together
input.mappings = gamecontrollerdb.txt   # optional: more pads for SDL's database
```

`one --list-gamepads` lists the connected pads, says whether SDL knows them (then the named buttons such as
`pad1:a`, `pad1:dpup` work) and prints the name of every button, stick or hat as you press it; raw names
(`pad1:button3`, `pad1:axis1-`, `pad1:hat0_up`) work for any pad. Mistakes in the file are reported as
`FILE:LINE: ...` (in the command window for the debugger), and that setting keeps its default.

### Sound

ONE plays sound through SDL2 at 48 kHz, mono. The five channels are mixed with the NES's non-linear formulas
(nesdev wiki "APU Mixer", the exact form), averaged over each output sample, and filtered like a front-loading NES
(high-pass 90 Hz and 440 Hz, low-pass 14 kHz; `audio.lowpass = off` leaves out the low-pass, as Mesen does, for a
slightly brighter sound and recordings that match Mesen's). A small, inaudible speed-up or slow-down (at most
0.5%) keeps the sound card's queue at `audio.latency` milliseconds, so the sound neither runs dry nor lags as the
sound card's clock drifts against the frame pacing. Settings in `one.cfg`:

```
audio.enabled = on          # off: silent (a --wav file is still written)
audio.volume = 50           # 0-100; the speakers only, a --wav file is always at full scale
audio.sample_rate = 48000
audio.latency = 50          # ms
audio.lowpass = on          # the NES's 14 kHz treble cut; off = like Mesen
audio.filters = nes         # none: no filters at all (fitting levels to a recording; tests)
```

`--wav FILE` writes the same sound (16-bit mono WAV, before the volume) also with `--headless`, which is how the
tests listen. Without a sound device or WAV file the APU skips sample generation, so traces and regressions run
at full speed. Sound is checked against Mesen in two ways (`tools/mesen/README.md` says how to record the
references): `--apu-log FILE` writes the APU's state once per frame in the format of Mesen's
`tools/mesen/apu_log.lua`, and `tools/apu_compare.py MESEN.txt ONE.txt` compares them field by field;
`tools/audio_check.py ONE.wav --reference MESEN.wav` compares the loudness of two recordings.

### Hotkeys and save states

These keys work in the game window (of `one` and of `one-debugger`):

| Key | |
|---|---|
| Esc | the menu (`one` only) |
| F1 | save the game to the current slot |
| F2 | load the game from the current slot |
| Shift+F1 / Shift+F2 | previous / next slot (1-9, wrapping around) |
| Shift+F12 | screenshot: the last complete frame as a 256x240 PNG next to the ROM (`game-001.png`, ...) |
| Shift+F10 | frames per second in the window title (about 60.1 at NES speed; 0.0 while the debugger is stopped) |
| Alt+Enter | full screen on/off (`video.fullscreen`; `video.scale` sets the window's size) |

`video.overscan` (0-16, the menu's Video page) hides that many pixels at each edge of the picture, as a TV's bezel
did: games left those edges untidy because no one saw them (SMB3's blank left column and the flicker at the left
of its status bar). `one` hides 8 by default; `one-debugger` shows the whole picture (default 0). The two share
`one.cfg`, so a `video.overscan` set there counts for both; `video.debugger_overscan` (0-16; empty = not set, the
default) gives `one-debugger` its own value, used instead of `video.overscan` (`one` ignores it). Only the window
is cropped: screenshots, traces and Lua's screen buffer keep all 256x240 pixels.

Save states go next to the ROM: `game.nes` -> `game.ss1` ... `game.ss9`. The current slot starts at the one saved
most recently. A message at the bottom of the picture says what happened ("Saved to slot 3", "Slot 4: empty",
"Slot 2: 2026-10-03 14:02" when changing slots...); it is not part of screenshots. There is no undo: saving
replaces the slot, loading replaces the game in progress. A state can only be loaded into the same game (the
ROM's CRC-32 is in the file) and by the same version of ONE (a different set of variables is refused). In the
debugger, F2 also works while the game is stopped (the windows then show the loaded state), F1 only while it runs.
From the command line, `--state-load FILE` starts from a state and `--state-save FILE --state-frame N` saves one
at frame N.
`--no-sav` neither reads nor writes the `.sav` file of a game with battery RAM (the Mesen trace tests need the game to start from
empty RAM: `regression/tests.txt`). `--sav-in FILE` starts the battery RAM from FILE and writes no `.sav` (a recording that began
from a save: `tools/mesen/ram_from_log.py` rebuilds the file from the trace).

A state is taken at the end of a frame, at most 1/60 s after the key is pressed: at that moment the CPU, PPU and
APU are each at a point where ordinary variables describe them completely (see `savestate.h`).
`tools/state_check.py` (ctest `savestate_fields`, label `lint`) checks that every variable of the emulator core is
either saved or marked as not machine state, and `tests/state_roundtrip.py` (ctest `savestate_roundtrip_*`) checks
that a loaded state continues exactly like the original run.

All the keys can be changed in `one.cfg`, with Shift/Ctrl/Alt in front of any key name; several keys are separated
by commas, `none` turns a hotkey off:

```
hotkey.state_save = key:f1
hotkey.fps = key:shift+f10, key:ctrl+alt+f
```

In the debugger, the game window passes only the keys that stop the game on to the debugger: F5, F8, F10, F12,
Ctrl+D and Alt+X. Shift+F12 and Shift+F10 also work in the debugger's own window (fixed there), and as the
commands `screenshot [file]` and `fps [on|off]`.
* Lag frame counter (FCEUX's): a frame in which the game did not read the controllers. "Lag counter on the picture" in
  the in-game menu, or the debugger's `lag on|off`, draws `Lag 123` at the top left of the game picture (red on a frame
  that lagged); `lag` prints the count, `lag reset` zeroes it (power-on does too, a soft reset does not). It is part of
  a save state. Lua: `one.lagCount()`, `emu.lagcount()`.

The window runs at NES speed (60.0988 frames/s, paced by ONE itself, independent of the monitor's refresh rate).
The CPU trace is off by default because it costs about 5x in speed; the regression scripts turn it on with `-t`.

## Lua scripts

`one --lua script.lua game.nes` (and `one-debugger --lua`, or its `lua` command) runs Lua 5.4 scripts written for
Mesen 2's Lua API (callbacks with `emu.addEventCallback()`, memory with `emu.read()` / `emu.write()`, drawing,
input, memory callbacks, save states) and for FCEUX's (`memory.readbyte`, `gui.text`, a main loop with
`emu.frameadvance()`): `lua/fceux.lua` provides FCEUX's names on top of Mesen's and is compiled into the binaries
(`cmake/embed.cmake`). Lua is downloaded and built with ONE (`ONE_LUA_GIT_TAG`, default v5.4.7); without a script
loaded it costs nothing measurable. What is implemented, and what is not yet, is in [lua.md](lua.md); the ctests
`lua_api`, `lua_api2` and `lua_fceux` (and their `debugger_` twins) show the APIs in use.

## PPU catch-up (speed)

`one` and the libretro core let the CPU run ahead of the PPU while nothing the CPU does can see the PPU, and catch the
PPU up when it matters: about 1.6 times faster, with exactly the same results.
It is the build feature `FEATURE_PPU_CATCHUP`: CMake `-DONE_PPU_CATCHUP=OFF` leaves it out of `one` and the libretro
core, `-DONE_PPU_CATCHUP_DEBUGGER=ON` puts it into one-debugger too (off by default there: the debugger needs the PPU
exact at every instruction, so it catches up at each one). While running, `ONE_NO_CATCHUP=1` in the environment turns
it off (to compare, or to look for a bug), `ONE_CATCHUP_STATS=1` prints how much ran ahead. Traces (`-t`) and Lua scripts
always run without it. `ctest -R ppu_catchup` compares the whole machine with and without it (`-L catchup`: every
ROM of `test_roms/`).
