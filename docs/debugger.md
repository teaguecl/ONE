# The debugger (`one-debugger`)

To build it see [BUILDING.md](../BUILDING.md); the game window, menu and settings it shares with `one` are in
[usage.md](usage.md). A quick reference card is in `docs/quickref/`.

A SoftICE-style debugger (registers, PPU, code, memory, OAM and command windows that can be moved, resized, zoomed
and closed), with the game in its own SDL2 window. The user interface is Dear ImGui, in a window of its own beside the
game window, drawn as a grid of characters. Ctrl+= and Ctrl+- (Window menu) make the characters
larger or smaller. In the command window, dragging with the mouse selects output (a double click: a word), in
the code window it selects the lines shown (a click still moves the cursor bar); Ctrl+C or Ctrl+Insert copies
the selection; Ctrl+V or Shift+Insert pastes onto the command line, and so does the middle
button (the X11 primary selection, which a selection here also sets). In the file dialogs (Open game, Run
script), typing while the list has the keyboard (Down from the name) jumps to the entry beginning with it.

```sh
cmake --preset linux-debugger
cmake --build --preset linux-debugger
./build/linux-debugger/debugger/one-debugger [-l game.dbg] [-s script.txt] [game.nes]
```

The game and debugger windows remember where they were when the debugger last quit (their places and sizes are kept in `~/.local/state/one/window-places.txt`; delete it to get the default places back). A place that is no longer on any screen is not used.

`ONE_DEBUGGER_CAPTURE=file.bmp` saves the ImGui window to that file whenever the debugger is waiting (stopped,
no commands queued), also with `SDL_VIDEODRIVER=dummy`: for tests and screen captures.

* Without a ROM (started from a file manager, say) the debugger opens with only the command window and the
  **File** menu's Open game dialog (`*.nes`); the File menu (Alt+F) also lists the 9 most recently opened games, the
  same list `one` keeps. `load FILE` (Tab completes it) loads another game at any time: its `.mlb` labels, `.watch`
  watches and `.init` commands come with it, breakpoints and frozen addresses are cleared, and unsaved labels are
  offered for saving first. Commands from `-s` wait until a game is loaded.

* Keys: F5 go / stop, F8 trace into, F7 go to the code cursor (HERE), F10 step over, F12 step to return, F9 toggle a breakpoint on the code
  window's cursor line (or double-click), Ctrl+D break, F4 the frame in progress (`rs`), F6 (or `ec`) enter/exit the code
  window as in SoftICE, Ctrl+F6 next window, Ctrl+F5 zoom,
  Ctrl+F7 move/size, Alt+X exit. Typing always goes to the command line; Tab completes a command name or a `cs` word.
  The game window's title says "ONE Debugger - Running" or "ONE Debugger - Stopped" (`one`'s says "ONE").
* Commands (`help` lists them, `help <cmd>` explains one): `t p g x here gsl bpx bpm bop bc bd be bl d dp u eb eo r n comment watch wc lb
  s cs freeze unfreeze lsave lload gg fs press savestate ec ww woam oam rs screenshot fps lag version log lua source load reset q`.
* `-l` loads labels: ld65 debug info (`ld65 --dbgfile game.dbg`, exact even in banked ROMs), Mesen `.mlb`, or
  ld65 `-Ln` / VICE label files. Labels can be used wherever an address is expected. `lsave` writes them (with
  the ones added with `n`) to the ROM's name with `.mlb` (`game.mlb`, Mesen's format), which is loaded
  automatically next time.
* Game Genie codes: `gg SXIOPO` (or `gg decode SXIOPO`) decodes a code, `gg 91d9 ad [compare]` makes one. Each game has a list of codes,
  each ticked or not: `gg on CODE [description]` (or `gg apply`) puts a code in it, ticked (the game reads the code's value at its
  address; an 8-letter code only where the ROM has its compare value, the way codes work in bank-switched games),
  `gg off CODE|*` unticks, `gg delete CODE|*` takes it out, `gg list` shows the list, `gg disable` / `gg enable`
  switches Game Genie off and on for the game (ticking a code switches it on). Memory views show what the CPU reads,
  codes included; `gg CODE` shows the ROM's own byte. Up to 64 codes; the real Game Genie took 3.
* `gg db` lists the known codes for the loaded game (from your own database made with `tools/cheats/build_db.py`, plus a few built in: `data/cheats/README.md`),
  found by the SHA-1 of the ROM data whatever the file is called; `gg db WORD` only those whose description has
  WORD (`gg db lives`), `gg db N` puts number N in the game's list, ticked.
* The list is remembered per game (not with `--batch`): `~/.config/one/cheats/<ROM name> [<CRC>].cht`
  (RetroArch's `.cht` format plus `gamegenie_enabled`), found by the game's No-Intro CRC-32, so a renamed ROM keeps
  its codes; it comes back when the game is loaded. `one` reads the same files (and will have a menu page for
  them); `one --cheat CODE` applies only the codes given, for that run, and changes no file.
* Frames and input: `fs [N]` runs N frames (1 by default) and stops at the start of vblank (scanline 241);
  `press [p2] BUTTONS [frames]` holds buttons (`a+right`, `start`; `-` for none) on pad 1 or 2 for the next frames
  (2 by default) while the game runs (`fs` or `x`); `savestate save|load [slot]` saves (at the next quiet point,
  scanline 240: it runs up to the next frame) or loads one of `one`'s save-state slots (0-9).
* Bank-switched games (MMC1, MMC3, MMC2): the code window's address column shows the PRG bank, `05:BAB7`; a
  breakpoint stop says where, "at 05:BAB7 (PRG bank 5 of 8, ROM offset $17AB7)"; the CPU memory window's title gives
  the bank of the address at its top. The registers line is unchanged.
* Code is shown starting at the instruction you ask for (`u`), and the code window lines up on PC and every
  instruction the game has executed. Going back from there (6502 code can be decoded from any byte) it picks, at
  each step, an executed instruction, else a documented opcode, else an unofficial one; bytes that cannot be an
  instruction without covering one of those show as `.db $xx` (`!assert previous` / `!assert backtrack` test it).
* Comments: `;` on the code window's cursor line (F6 first) puts `comment ADDRESS <the comment there now>` on the
  command line to edit; Enter saves it, an empty text removes it. Comments are shown after the instruction on one
  line (line breaks, `\n`, become spaces; cut off at the window's edge) and kept with the labels in the `.mlb` file
  (Mesen's comment field, so Mesen shows them too); `lsave` saves them.
* Watches (`watch 774 b health`; types `b w s bin`, `l n` for n bytes, `-p` for PPU memory) are shown in the watch
  window (8), to the right of the code window (which gets narrower while it is open). It opens when a watch is
  added (or with `ww`) and closes when `wc` removes the last one. Values are
  read at every stop and highlighted when they changed. In the window, Del removes a watch, Enter puts `eb` for it
  on the command line, Ins `watch`, Ctrl+F freezes it. The list is kept in the ROM's name with `.watch`
  (`game.watch`, watch commands) and loaded next time; `--batch` neither reads nor writes it.
* Lua: `lua FILE` runs a script; relative names are from the directory the debugger was started in (`lua` alone
  lists the running scripts and says which directory that is), `~` is the home directory, and spaces need no
  quotes (`lua my hud.lua`; quotes work too). Tab completes the file name and the hint line lists the matches.
  The **Lua** menu (Alt+L) has Run script... (a file dialog, starting in the ROM's directory), Stop all scripts and
  List running scripts; `lua stop N` ends one script.
* The OAM window (`woam`, 7) lists the sprites decoded: Y, X, tile (both tiles of an 8x16 sprite), palette,
  priority (`fg` / `bg`: in front of / behind the background), flips, the picture lines it covers, and notes:
  sprite 0 and where its hit happened, "dropped on lines ..." where it is a 9th sprite on a line (the PPU draws
  only 8), hidden (below the picture), partly below the picture, left 8 pixels masked. The header says the sprite
  size and pattern table, where the last OAM DMA copied the sprites from and when, how many are visible and which
  lines have too many. Values changed since the last stop are highlighted. Keys: A all 64 sprites / the visible
  ones, H the bytes / decoded, Enter puts `eo` for the sprite on the command line. `oam [all]` prints the same;
  `eo offset = values` edits OAM (a game that copies its sprites with DMA overwrites them the next frame: edit its
  copy with `eb`).
* `rs` (F4) switches the game window, at every stop, to the frame in progress: the pixels the PPU has drawn so far
  as they are, the rest (still the previous frame) dimmed, and the beam marked (white, with gold ticks at both ends
  of its line); the title says "frame so far, line N". Stepping with `t` or `gsl` then shows mid-frame effects
  (split scrolling, sprite 0 hits, IRQ status bars) line by line. `rs` again: the last complete frame.
  (The name is to be revisited.)
* The emulator's own output and the trace (`-t`) go to `one-debugger.log` (`--log FILE`), not to the terminal.
* Settings (window shadows, `shadows = off` by default: menus and dialogs are shadowed, windows are not; and all the colors) come from a plain-text `key = value` file: `--config FILE`, else
  `./one-debugger.cfg`, else `~/.config/one/debugger.cfg`. There are two built-in color themes, `softice` (the
  default: black, from SoftICE's COLOR defaults) and `borland` (Turbo Vision blue): choose one with `--theme NAME`
  or `theme = borland` in the file, and override single colors below it. `debugger/one-debugger.cfg` and
  `debugger/one-debugger-borland.cfg` list every setting of each theme with a description; regenerate them with
  `one-debugger --write-config FILE [--theme borland]`. Colors are `fg on bg`
  with the 16 classic names, `#RRGGBB` or `default`, plus optional `bold`/`italic`/`underline`. Mistakes are
  reported, with line numbers, in the command window.
* `wbg` opens the background window (both user interfaces): the four nametables (512x480, through the cartridge's
  mirroring) drawn with the background pattern table and palettes as they are now, and the part the game window
  shows, at full brightness with a white outline, the rest dimmed. Each picture line is placed by the scroll it
  started with, so status bars and other mid-frame scroll changes show up as separate pieces, and with the CHR
  banks it was drawn with (games switch them mid-frame); the rest of the map uses the banks most lines used. A view
  scrolled across the map's edge wraps around to the other side, as on the NES. O in the window switches the outline
  to a single 256x240 rectangle from the scroll the picture starts with (as Mesen draws it), and back; the title
  says which. It is live while the
  game runs (a frame at a time) and opens where it was last time. `wbg save file.png` writes the view as a PNG (also
  in `--batch`).
* `wpal` opens the palette window (after Mesen's Palette Viewer): palette RAM (BG0-BG3, SP0-SP3, with their values;
  $3F10/$3F14/$3F18/$3F1C, the same bytes as $3F00/$3F04/$3F08/$3F0C, marked in a corner) and the NES's 64 colors,
  the ones palette RAM uses outlined; the bottom line and the title say what the mouse is over. Live like `wbg`, and
  it opens where it was last time. `wpal save file.png` writes it (also in `--batch`).
* `wpat` opens the pattern table window (after Mesen's Tile Viewer): both pattern tables side by side ($0000, $1000)
  through the CHR banks of now, labelled with what PPUCTRL uses them for (BG, SP). Keys in it: P switches the palette
  (BG0-BG3, SP0-SP3, gray; BG0 to start with), G a tile grid; the bottom line and the title say which tile the mouse
  is over (number, address, CHR-ROM offset). Live, remembered place, `wpat save file.png`.
* `docs/quickref/one-debugger-quickref.pdf` is a two-page quick reference card, generated from the debugger's
  source (`cmake --build <dir> --target quickref`, or `docs/quickref/build.sh`; see `docs/quickref/README.md`).
  Help > Quick Reference in the debugger opens it (with `xdg-open`); `cmake --install` puts it in `share/doc/one`.
* `handback_button = guide` in the debugger's config file: a gamepad button (SDL's names: `guide`, `back`, `misc1`,
  `paddle1`...; default `none`) that stops the game like Ctrl+D, for handing the game back to an AI agent
  (`wait_for_user`). Pick one the game's controls do not use.
* `--remote [PORT]`: the remote-control channel for the MCP server (`tools/mcp/one_mcp.py`: an AI agent drives
  the debugger; setup in `tools/mcp/README.md`). It listens on a Unix socket in `$XDG_RUNTIME_DIR/one-debugger/` (with a port: TCP on
  127.0.0.1 only; `0` picks a free one; on Windows always TCP, files in `%LOCALAPPDATA%\one-debugger\`) and prints `remote: listening on <where>, token <file>` on stdout. A client
  first sends `{"auth": "<the token file's contents>"}` (a new token each run, mode 0600; no token, a wrong one or
  none within 5 s closes the connection; one client at a time), then one JSON request per line:
  `{"id": 1, "cmd": "fs 10"}` (a command, while stopped; one that runs the game replies when it stops, or after
  `"timeout"` seconds), `{"id": 2, "op": "pause"}`, `"wait"`, `"screenshot"` (a base64 PNG), `"state"`, `"read"`
  (memory; PRG-ROM by bank), `"disassemble"`, `"rominfo"` or `"notify"` (a message to the player; requests: `debugger/remote.hpp`). Every reply
  has the state (running, pc, frame, scanline, cycle, rom). Only safe commands run: no `lua`, `source`, `lb`,
  `screenshot FILE`, `lsave`/`lload`/`wbg`/`wpal`/`wpat` with a file; breakpoint conditions only with registers,
  `mem[]`, numbers and operators; breakpoint actions without `load` or breakpoint commands; `load` only from the
  start-up ROM's directory or `--remote-roms DIR` (repeatable). The commands show in the command window as if typed.
  With `--batch` there is no user interface: the script runs, then the channel serves requests until `q`.
* `--batch -s script.txt` runs a command script without the UI and prints the results; `!assert reg pc c000` /
  `!assert mem 0300 12` (also `frame N`, `scanline N`, `pad 1|2 BUTTONS`) lines make the exit code 1 when they fail (see `tests/debugger_smoke.txt`).

## Finding Game Genie codes with Cheat Search

A Game Genie changes what the CPU reads from the cartridge ROM ($8000-$FFFF), so a code is a patch to one ROM byte. The
game's variables (lives, health, time) are in RAM, so the work is in three steps: find the variable (`cs`), find the
ROM byte that sets or changes it (`bpm`, `u`), and make the code (`gg`). The example below is for "lives"; the
addresses in it are made up.

**1. Find the variable's address.** `cs` (cheat search) keeps a list of candidate bytes and drops those that do not fit
what you saw the game do. The values are hexadecimal.

```
cs start                  every byte of CPU RAM ($0000-$07FF) and cartridge RAM ($6000-$7FFF) is a candidate
                          (cs start ppu | oam searches video memory or sprite memory)
(play with F5, or `fs N` to run N frames; Ctrl+D or a breakpoint stops the game)
cs = 3                    the game shows 3 lives: keep the bytes that are 3 now
(lose a life, stop the game)
cs - 1                    keep the bytes that went down by exactly 1 while the game ran
cs = 2                    ... and are 2 now
cs list                   the candidates, 16 at a time: address, value, was, and "written by $ADDR" (the instruction
                          that wrote the byte while the game ran)
```

Repeat until one or a few bytes remain. Other filters: `cs !=`, `cs <`, `cs >`, `cs +n`, `cs same`, `cs diff`, `cs up`,
`cs down`, and `cs written` / `cs notwritten` / `cs read` / `cs notread` (which bytes the CPU touched, handy to throw
away screen counters and timers). Filters typed one after another judge the same stretch of play. `cs undo` takes back
the last filter, `cs reset` starts over on the same memory, `cs signed` compares as signed bytes, `cs stop` ends the
search. For a value you cannot see (a hidden counter) use only `cs same` while nothing happens and `cs diff` / `cs up` /
`cs down` after something does.

**2. Check it.** `freeze 075A 9` writes 9 into that byte every frame; if the game then shows 9 lives and does not lose
them, the byte is right. `unfreeze 075A` (or `unfreeze *`) lets go. `watch 75A b lives` keeps it in the watch window.

**3. Find the ROM byte to patch.** Stop the game when the variable is written: `bpm 075A w` breaks after the instruction
that wrote it, and `u` (or the code window) shows it. Two common cases:

* *The starting value*: `LDA #$03` then `STA $075A`. The byte to patch is the `03` of the `LDA`. A code that changes it
  (`03` to `09`) gives 9 lives from the start.
* *The decrement*: `DEC $075A` (opcode `CE`). Replacing the opcode with another of the same length that reads the byte
  and changes nothing, such as `LDA $075A` (`AD`), often stops the game from losing lives. Read the opcode's address
  and byte in the code window (`u`); check that the registers and flags the game uses afterwards still make sense.

Where a byte sits in the ROM matters for bank-switched games: the code window shows the bank (`05:BAB7`), and the
stop message gives the ROM offset.

**4. Make the code.** `gg ADDRESS VALUE [COMPARE]` encodes the patch: the CPU address of the ROM byte, the byte to
read instead, and, for an 8-letter code, the byte that is there now.

```
gg 91d9 ad                $91D9 = AD: SXIOPO          a 6-letter code: always replaces the byte at $91D9
gg 91d9 ad 05             $91D9 = AD if 05: SXSOPPIE  an 8-letter code: only while the byte there is 05
```

Use the 8-letter form in bank-switched games (MMC1, MMC3 and the like): the same address shows different ROM bytes in
different banks, and the compare byte keeps the code to the bank you found it in. The real Game Genie did the same.
Then `gg on SXSOPPIE infinite lives` puts it in the game's list, ticked. Check it in the game, `gg off` it to compare,
`gg list` shows the list. `gg SXSOPPIE` decodes any code back to address, value and compare.

**5. Keep it.** The list is saved per game in `~/.config/one/cheats/<ROM name> [<CRC>].cht` (see above) and `one` reads
it too. To share a code, add it to a game entry in your own `nes_gamegenie.txt` (the format is in `data/cheats/README.md`:
`@<SHA-1 of the ROM data> <name>`, then `<code><TAB><description>`), or send it with its description.

## The MCP server: letting an AI agent use the debugger

`tools/mcp/one_mcp.py` is an MCP server (Model Context Protocol) that gives an AI agent (Claude Code, or any MCP client)
the debugger's abilities: load a game, run frames, press buttons, look at the screen, read and search memory, set
breakpoints, disassemble, read the call stack, and make Game Genie codes. It talks to a running `one-debugger` over
the debugger's remote channel (`--remote`, below), so the agent and you see the same game.

**Setup** (Linux):

```sh
python3 -m venv ~/.local/share/one/mcp-venv                 # Ubuntu's Python refuses pip installs: use a virtual environment
~/.local/share/one/mcp-venv/bin/pip install mcp             # the official MCP SDK
cmake --preset linux-debugger && cmake --build --preset linux-debugger
claude mcp add one-debugger -- ~/.local/share/one/mcp-venv/bin/python /path/to/ONE/tools/mcp/one_mcp.py
```

Use absolute paths (the agent may start in another directory). The new tools show up in a new Claude Code session (or
after `/mcp`). `claude mcp list` says whether the server connects; running `python3 tools/mcp/one_mcp.py` by hand
shows why it does not. Other MCP clients start `python one_mcp.py` the same way, over stdio.

Options, after the script's path: `--headless` (no windows, the agent works alone), `--attach [PID]` (only use a
debugger you started yourself), `--rom-dir DIR` (where the agent may load games from, repeatable; by default the
directory of the first game) and `--debugger PATH` (the `one-debugger` to start; default the build tree's, then
`$ONE_DEBUGGER`, then `PATH`).

**Two ways to work**

* *The agent starts the debugger:* it calls `load_rom("/path/game.nes")`, which starts `one-debugger --remote` with the
  game. The windows open, so you can watch. The debugger ends when the MCP server does.
* *You start it, the agent joins:* run `one-debugger --remote game.nes`, play in the game window, then ask the agent
  to look. It works between your turns. With several debuggers running, each tool takes an optional `pid`
  (`list_debuggers` shows them); without one it says which pids there are.

**Handing the game back and forth.** When the agent needs you to do something in the game (reach a level, lose a life),
it calls `wait_for_user("message")`. The game stops and stays stopped; the message appears in the command window and on
the game picture. Press **F5** in the game window to start the game, do what it asked, then press **F5** again (or
Ctrl+D, or the gamepad button set by `handback_button` in the debugger's config file) to stop it. The agent continues
from there. The agent cannot start the game for you.

**The tools** (the agent sees them with descriptions; `debugger_command` runs any other safe debugger command):

| Tool | What it does |
|---|---|
| `load_rom`, `reset` | load a game (starts the debugger); RESET / power cycle |
| `get_status`, `pause`, `resume`, `run_frames`, `step` | registers and state; stop; run; run N frames; step into / over / out |
| `set_controller`, `set_zapper` | hold buttons for N frames; aim the Zapper and fire |
| `save_state`, `load_state`, `screenshot` | save-state slots 1-9; the picture as an image |
| `read_memory`, `write_memory`, `rom_info`, `find_in_rom` | CPU, PPU, OAM and palette memory; PRG-ROM by bank; find bytes in every bank |
| `disassemble`, `label`, `backtrace`, `cdl` | code with labels and banks; name an address; the call stack; the code/data log |
| `set_breakpoint`, `remove_breakpoint`, `list_breakpoints`, `wait_for_breakpoint` | execution, read, write, opcode breakpoints with conditions |
| `cheat_search`, `game_genie`, `set_cheats` | the `cs` search; encode / decode codes (in bank-switched games it says in which banks a code acts); the known codes for the game |
| `wait_for_user` | show a message with the game stopped; you act and hand it back |

Two resources help the agent: `one://help` (the debugger's commands) and `one://guide/game-genie` (finding a RAM
variable and making a code).

**Using it.** Say what you want in plain words, and let the agent drive: "load Ninja Gaiden, find player 1's health and
make a Game Genie code that keeps it full", "why does the screen flicker at scanline 120?", "trace what calls the
routine at $C123". Give it a way to see (`screenshot`) and, when it needs the game in a state it cannot reach alone,
let it ask you (`wait_for_user`). Ask for the commands it ran if you want to repeat them in the command window: they
show there as if typed.

**Safety.** The remote channel is a socket only your user can open, protected by a token that is new each run (the
token file is created with mode 0600; a wrong or missing token closes the connection). Only a list of safe commands
runs: no Lua scripts, no reading or writing files the agent names, breakpoint conditions without Lua functions, and
games only from the ROM directories. Do not point an agent at games or directories you would not let it read.

The server's own notes and its tests (`tests/mcp_check.py`, `tests/health_check.py`, ctest `debugger_mcp` and
`debugger_mcp_health`, which are skipped when the Python CMake finds has no MCP SDK) are in `tools/mcp/README.md`.
# Code
The debugger is C++: it lives in `debugger/` and links the
emulator core, which is built a second time with `ONE_DEBUGGER` defined (`one_core_debug`) to add the
instruction-boundary and memory-access hooks (`debug_hooks.h`). `one` itself is built without them. CMake refuses to
configure if a C++ file appears in the core.
