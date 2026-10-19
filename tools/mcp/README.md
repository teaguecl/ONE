# ONE's MCP server: one-debugger for AI agents

`one_mcp.py` lets an AI agent (Claude Code, or any MCP client) use `one-debugger`: load a game, run frames, press
buttons, look at the screen, read and search memory, set breakpoints, disassemble, and make Game Genie codes.

## Setup

    python3 -m venv ~/.local/share/one/mcp-venv          # the official MCP SDK (version 1 or 2); Ubuntu's own
    ~/.local/share/one/mcp-venv/bin/pip install mcp       # Python refuses pip installs, hence a virtual environment
    cmake --preset linux-debugger && cmake --build --preset linux-debugger
    claude mcp add one-debugger -- ~/.local/share/one/mcp-venv/bin/python /path/to/ONE/tools/mcp/one_mcp.py

Use absolute paths (Claude Code may start elsewhere). New tools appear in a new Claude Code session (or after
`/mcp`). `claude mcp list` says whether it connects; `python3 tools/mcp/one_mcp.py` run by hand says why not.
For the ctests `debugger_mcp` and `debugger_mcp_health` to run (not skip), configure with
`-DPython3_EXECUTABLE=$HOME/.local/share/one/mcp-venv/bin/python`.

Options (after the path): `--headless` (no windows: the agent alone), `--attach [PID]` (only use a debugger the user
started), `--rom-dir DIR` (where `load_rom` may load games from; default the first game's directory),
`--debugger PATH` (default `build/linux-debugger/debugger/one-debugger`, `$ONE_DEBUGGER`, or the one on `PATH`).

## Several debuggers at once

Every tool takes an optional `pid` (which `one-debugger --remote` to use; `list_debuggers` shows them). Without it
a tool uses the only debugger running, the one of `--attach PID`, or the one `load_rom` started. With two running and
no `pid`, a tool says so and lists the pids. Calls to different pids can run in parallel.

## Two ways to work

- **The agent starts the debugger:** `load_rom("/path/game.nes")` launches `one-debugger --remote` with the game
  (its windows open, so you can watch). It ends when the MCP server does.
- **You start it, the agent joins:** `one-debugger --remote game.nes`, then any tool connects to it. You can play
  in the game window; the agent watches and works between your turns.

Handing the game to you: the agent calls `wait_for_user("...")`. The game is stopped and stays stopped: the message
appears in the command window (and a prompt on the game picture), so you can read it without the game moving. Press
**F5** in the game window to start the game, do what it asked, then press **F5** (or Ctrl+D, or the gamepad button
set with `handback_button = guide` in the debugger's config file) to stop it. The agent continues from there. This
is the only behavior: the agent cannot start the game for you.

## Tools

| Tool | What it does |
|---|---|
| `load_rom`, `reset` | load a game (or start the debugger with it); RESET / power cycle |
| `get_status`, `pause`, `resume` | registers and state; stop; run |
| `run_frames`, `step` | run N frames (stops at vblank); step into / over / out |
| `set_controller` | hold buttons for N frames (and run them) |
| `set_zapper` | aim the Zapper (light gun, port 2) at a pixel, pull the trigger for N frames; unplug it |
| `save_state`, `load_state` | the emulator's save-state slots 1-9 |
| `screenshot` | the picture, as an image |
| `read_memory`, `write_memory` | CPU, PPU, OAM, palette memory; PRG-ROM by bank, mapped in or not |
| `disassemble`, `label` | code with labels and PRG banks (any bank); name an address |
| `rom_info`, `find_in_rom` | PRG-ROM banks and which are mapped; find bytes in every bank |
| `set_breakpoint`, `remove_breakpoint`, `list_breakpoints`, `wait_for_breakpoint` | exec / read / write / access / opcode, with conditions |
| `cheat_search` | find a variable by how it changes (the debugger's `cs`) |
| `backtrace` | the call stack: how the CPU got here (`bt`; `jumps`: the last jumps and branches) |
| `cdl` | the code/data log, Mesen's CDL format: `on`, `stats`, `unseen`, `save`, `load` |
| `game_genie`, `set_cheats` | decode / encode / apply codes (`gg`), the known codes for the game; in bank-switched games, says in which banks a code acts |
| `wait_for_user` | show a message with the game stopped; you start it (F5), act, stop it (F5) |
| `debugger_command` | any other allowed debugger command |

Resources: `one://help` (the debugger's commands), `one://guide/game-genie` (finding a RAM variable, making a
code).

## Safety

The server talks to the debugger over its remote channel (`debugger/remote.hpp`): a socket only you can open (on
Windows TCP on 127.0.0.1, where only the token keeps others out), and a new token each run. Only a list of safe commands runs: no Lua scripts, no reading or writing files the agent
names, breakpoint conditions without Lua functions, games only from the ROM directories.

## Tests

    python3 tests/mcp_check.py build/linux-debugger/debugger/one-debugger tests/data/nestest.nes
    python3 tests/health_check.py build/linux-debugger/debugger/one-debugger tests/health/health.nes

`mcp_check.py` calls every tool. `health_check.py` is a scripted agent: in `tests/health/health.nes` (a tiny MMC1
game: A is a hit, decoy variables change on their own) it finds the health byte with `cheat_search`, the `DEC` in
bank 1 with a write breakpoint and `disassemble`, makes the 8-letter code with bank 1's compare value, and checks
that hits no longer cost health. (ctest `debugger_mcp`, `debugger_mcp_health`; skipped when the Python CMake found
has no MCP SDK.)

By hand, with an agent: Ninja Gaiden (MMC1, 8 banks) -- "find player 1's health and make a code that keeps it".
The known codes (`game_genie("db")`) give something to compare with.
