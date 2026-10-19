# Lua scripts

ONE runs Lua 5.4 scripts written for [Mesen 2's Lua API](https://www.mesen.ca/docs/apireference.html): a script
registers callbacks with `emu.addEventCallback()` and reads and writes the machine with `emu.read()`,
`emu.write()`, `emu.getState()` and so on, using Mesen's names (`emu.memType.nesMemory`,
`emu.eventType.endFrame`). A script that uses only what is listed below runs unchanged.

Scripts written for **FCEUX** run too (see [FCEUX scripts](#fceux-scripts)): `memory.readbyte`, `gui.text`,
`joypad.set`, a main loop that calls `emu.frameadvance()`, and Lua 5.1 names such as `unpack` and `bit.band`.

In ONE, the menu's **Lua scripts** page (Esc, then L) starts, restarts and stops scripts while a game runs, and
keeps a list of the recent ones. From the command line:

```sh
one --lua hud.lua game.nes                 # up to 8 scripts, started in order
one-debugger --lua hud.lua game.nes        # or the debugger's lua command: lua hud.lua / lua / lua stop
```

`print()` and `emu.log()` go to the console (the debugger: its command window); errors are reported with a stack
trace, and a script with an error is ended. Each script has a Lua state of its own and the full standard library
(`io` for writing files). `require` also finds modules next to the script. With no script loaded the emulator
runs at full speed: every event site costs one predictable branch.

## Implemented (Mesen 2 names)

| Function | Notes |
|---|---|
| `emu.addEventCallback(function, eventType)` | returns a reference for `removeEventCallback` |
| `emu.removeEventCallback(reference, eventType)` | |
| `emu.read(address, memType, signed)` | memType defaults to `nesMemory`; reads never have side effects (reading `$2002` does not clear VBL) |
| `emu.readWord(address, memType, signed)` | little-endian |
| `emu.write(address, value, memType)` / `emu.writeWord(...)` | writable: internal RAM, work/save RAM, sprite RAM, secondary sprite RAM, palette; others are ignored with a warning |
| `emu.getState()` | flat table: `cpu.a x y sp ps pc cycleCount`, `ppu.scanline cycle frameCount control mask status videoRamAddr tmpVideoRamAddr xScroll writeToggle`, `frameCount`; the pre-render line is scanline -1 as in Mesen |
| `emu.getRomInfo()` | `name`, `path`, `fileCrc32Hash` |
| `emu.getScriptDataFolder()` | `~/.local/share/one/lua/<script>` (`$XDG_DATA_HOME`; `%APPDATA%\one\lua` on Windows), created when asked for |
| `emu.log(...)`, `print(...)` | |
| `emu.displayMessage(category, text)` | on the game window for a few seconds |
| `emu.stop(exitCode)` | `one` stops and exits with `exitCode`; the debugger quits |

**Events** (`emu.eventType`): `reset` (after the reset vector is read), `nmi` and `irq` (when taken; PC is the
handler), `startFrame` (pre-render line, dot 0), `endFrame` (the picture is done: scanline 239, dot 338, where
Mesen's endFrame sees the CPU cycle counter), `inputPolled` (once a frame, after the controllers are read from
the keyboard and gamepads), `stateLoaded`, `stateSaved`, `scriptEnded` (to the script that ends), `codeBreak`
(the debugger stopped).

**Memory types** (`emu.memType`): `nesMemory` (CPU address space), `nesPpuMemory`, `nesPrgRom`,
`nesInternalRam`, `nesWorkRam`, `nesSaveRam`, `nesSpriteRam`, `nesSecondarySpriteRam`, `nesPaletteRam`,
`nesChrRom`, `nesChrRam`. `emu.callbackType` (`read`, `write`, `exec`) and `emu.cpuType.nes` exist for scripts
that name them.

**Drawing** happens over the picture in the game window (the picture itself, screenshots and recordings stay as
the game made them). Colors are Mesen's: `0xAARRGGBB` with the alpha inverted, so `0x00FF0000` is opaque red and
`0x80FF0000` half-transparent red; `0xFF......` is invisible.

| Function | Notes |
|---|---|
| `emu.drawPixel(x, y, color, duration, delay)` | duration: frames it stays (default 1: this frame); delay: frames before it appears |
| `emu.drawLine(x, y, x2, y2, color, duration, delay)` | |
| `emu.drawRectangle(x, y, width, height, color, fill, duration, delay)` | |
| `emu.drawString(x, y, text, textColor, backgroundColor, maxWidth, duration, delay)` | ONE's 5x7 font, 6x9 pixels a character; `\n` and maxWidth (pixels) start new lines |
| `emu.clearScreen()` | every drawing goes |
| `emu.getPixel(x, y)` / `emu.getScreenBuffer()` | the picture (`0xRRGGBB`); the buffer is 256x240 colors, row by row |
| `emu.setScreenBuffer(colors)` | a whole picture over the game's, for one frame |

**Input**

| Function | Notes |
|---|---|
| `emu.getInput(port)` | `{a, b, select, start, up, down, left, right}`, booleans; port 0 or 1. With a Zapper on port 1 (`input.zapper`, `--input` aim lines, `zapper` in the debugger): `{x, y, trigger}` as in Mesen (x, y = -1 away from the screen) |
| `emu.setInput(input, port)` | from an `inputPolled` callback: the buttons the game reads this frame; buttons the table does not name stay as the player has them. Port 1 with `x`, `y` and / or `trigger` aims the Zapper (plugs it in) for this frame (Mesen 2.2.1 ignores that for its Zapper; ONE does not) |
| `emu.isKeyPressed(name)` | SDL key names: `"A"`, `"Space"`, `"Left"`, `"F1"`, `"Left Shift"`, ... |
| `emu.getMouseState()` | `{x, y, left, right, middle}`, in NES pixels |

**Memory callbacks**: `emu.addMemoryCallback(function, callbackType, startAddress, endAddress)` (end defaults to
start) calls `function(address, value)` for each CPU read, write or instruction fetch (`exec`) in the range; for a
read, a number returned replaces the value the CPU gets. Only `nesMemory` (the CPU bus) is supported.
`emu.removeMemoryCallback(reference, callbackType, startAddress, endAddress)` removes one. With no memory callback
the CPU pays nothing more than with no script.

**Save states and the machine**

| Function | Notes |
|---|---|
| `emu.createSavestate()` | returns a state object (Mesen returns a string); it is filled at the end of the picture of this frame -- the only point where ONE can take a state -- and can be loaded after that |
| `emu.loadSavestate(state)` | back to that state at the next instruction; `stateLoaded` events follow |
| `emu.setState(table)` | sets the CPU registers it names: `cpu.a`, `cpu.x`, `cpu.y`, `cpu.sp`, `cpu.ps`, `cpu.pc` |
| `emu.reset()` | the RESET button, at the next instruction |

**Windows of their own** (`one.*`, script_window.c): `one.openWindow(title, width, height[, scale])` returns an id (or nil
and why); the window shows the picture SCALE times its size (0.5, 1, 2...; without it: twice if that fits); `one.setWindowPixels(id, pixels)` shows a picture (a table of width x height 0xRRGGBB colors, row by row);
`one.windowKeys(id)` lists the keys pressed in it since the last call (SDL names: "O", "Left"), nil once the user
closed it; `one.windowMouse(id)` gives x, y (the picture's pixels) and buttons (1 left, 2 middle, 4 right) of the
mouse over it, nil when it is not; `one.saveWindow(id, file.png)`; `one.closeWindow(id)`. A script's windows close when it ends. Headless
(or `--batch`) they exist as pictures only. **`one.features()`**: `{program = "one" | "one-debugger", debugger =
true when debugger.command() / emu.breakExecution() / emu.getLabelAddress() are there, windows = true when windows
can be seen}`, for scripts that adapt to where they run. `lua/examples/background.lua` uses them (see
`lua/examples/README.md`).

**ONE's own** (`one.*`): `lagCount()` (frames in which the game did not read the controllers, since power-on),
`isLagFrame()` (was the last frame one), `resetLagCount()`,
`newState()` / `saveState(state)` / `stateReady(state)`, `paletteColor(index)` (`0xRRGGBB` of NES color 0-63),
`screenPaletteIndex(x, y)`, `message(text)`, `screenshot([file])`, `apuState()` (the sound channels' registers and
counters), `hudPixel(x, y)` (the drawing layer as last shown, `0xAARRGGBB` with 255 = opaque).

## FCEUX scripts

Every script runs with a layer ([lua/fceux.lua](../lua/fceux.lua), built into ONE) that provides FCEUX's Lua API on
top of the functions above. The script's main chunk runs as a coroutine: `emu.frameadvance()` suspends it until the
end of the next frame, so FCEUX's

```lua
while true do
  gui.text(8, 8, "Lives: " .. memory.readbyte(0x075A))
  emu.frameadvance()
end
```

works as written. (A Mesen style script never calls `emu.frameadvance()` and is not affected; the two styles can be
mixed.) `require` finds the helper libraries FCEUX's scripts come with (`x_functions.lua`, `shapedefs.lua`, ...)
when they are next to the script.

| FCEUX | In ONE |
|---|---|
| `emu.*` | `frameadvance`, `framecount`, `lagcount`, `lagged`, `setlagflag`, `registerbefore` (start of a frame), `registerafter` (end of the picture), `registerexit`, `message`, `print`, `getscreenpixel`, `softreset`/`poweron` (both a RESET), `getdir`, `exit`; `pause`, `speedmode`, `loadrom`, Game Genie: no effect |
| `memory.*` | `readbyte`/`readbytesigned`/`readword`/`readwordsigned`/`readbyterange`, `writebyte`, `getregister`/`setregister`, `registerwrite`/`registerread`/`registerexec` (`function(address, size, value)`, for the CPU's accesses) |
| `rom.*`, `ppu.*` | `rom.readbyte` reads the ROM file (iNES header included, as FCEUX); `rom.gethash` is a CRC-32, not MD5; `ppu.readbyte`/`readbyterange` |
| `joypad.*` | `get`/`getdown`/`getup`/`getimmediate`, `set` (true / false / `"invert"` / nil, for the next frame) |
| `input.get()`, `zapper.read()` | keyboard (FCEUX's key names) and mouse |
| `zapper.set{x=, y=, fire=}` | aim the Zapper this frame (also `xmouse`, `ymouse`, `click`); x, y outside the picture: away from the screen |
| `gui.*` | `pixel`, `line`, `box`, `text`, `getpixel`, `parsecolor`, `opacity`, `transparency`, `register`, `savescreenshot[as]`, `gdscreenshot`, `gdoverlay`, `popup` (prints) |
| `savestate.*` | `object`/`create`, `save`, `load`, `registersave`, `registerload`: state objects as above (numbered slots are not supported) |
| `sound.get()` | the 2A03 channels: volume, frequency, MIDI key, duty, registers |
| `bit.*`, `AND`/`OR`/`XOR`/`BIT`/`SHIFT`, `tobitstring`, `copytable` | |
| Lua 5.1 | `unpack`, `loadstring`, `table.getn`/`maxn`/`foreach`, `math.pow`/`mod`/`log10`/`atan2`, `string.gfind`; `string.format("%d", 2.5)` truncates, `table.remove` past the end does nothing, as in 5.1 |
| `movie.*`, `taseditor.*` | stubs: ONE has no movies |
| `require("auxlib")`, IUP | a stand-in: the script runs, its windows (IUP dialogs) are not shown |

Differences from FCEUX: colors are FCEUX's (`"#rrggbbaa"`, names, `"P1A"`, `0xRRGGBBAA`, tables); text uses ONE's
font, 6 pixels a character where FCEUX's is proportional (about 5), so text laid out tightly may overlap;
`gui.getpixel` reads what was drawn for the frame before; scripts that need Windows (`winapi`) do not run.

## Not yet

`emu.breakExecution`/`resume`/`execute`, labels (`getLabelAddress`), memory callbacks on memory types other than
`nesMemory`, nametable RAM as a memory type, `emu.getState()`'s APU and mapper keys, FCEUX's numbered save state
slots and movies.
