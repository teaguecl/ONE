# Changelog

The version number is explained in [CONTRIBUTING.md](CONTRIBUTING.md#releases-and-the-version-number).

## 0.2.0 (not released yet)

* RAMBO-1 (mapper 64: Klax, Shinobi, Skull & Crossbones, Rolling Thunder, Xybots and others), written from hardware
  measurements of the chip. It passes the six hardware test ROMs, which are now in `tests/rambo1hw`, and runs with PPU
  catch-up (about 1.5 to 2.4 times faster than without). Dig Dug II (Japan) now runs as mapper 206, as it is.
* Debugger: breakpoints can name a bank (`bpx BB:ADDRESS`), `bpm` takes a range (`bpm START..END`), `n` and `comment`
  take `BB:ADDRESS`, and F9 sets a breakpoint in the bank mapped now. Up to 64 breakpoints (was 16).
  `remove_breakpoint` in the MCP server needs a breakpoint number (it no longer removes all when given an id).
* MCP: `prof` works over the remote channel, and there is a `profile` tool.

## 0.1.0 (2026-10-24)

* First public version: the emulator (`one`), the debugger (`one-debugger`), the Mesen 2 and FCEUX Lua APIs, the
  in-game menu, and the libretro core.
* AccuracyCoin: 144 of 144 tests pass.
* A version number: `one --version`, `one-debugger --version` and the `version` command, the menu, the libretro core
  and Lua's `one.version()`; builds that are not a tagged release show the commit.
