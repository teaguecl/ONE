# Example Lua scripts

Scripts that show what ONE's Lua API can do (the API: `docs/lua.md`). Run one with `one --lua SCRIPT game.nes`, or in
one-debugger with the `lua SCRIPT` command (or Lua > Run script...).

| Script | What it shows |
|---|---|
| `background.lua` | The four nametables in a window of its own (`one.openWindow`), the part the game window shows outlined and the rest dimmed; O in the window: the outline line by line or as one rectangle. It rebuilds each picture line's scroll from the game's writes to the PPU registers (`emu.addMemoryCallback`), as the PPU applies them. `SCALE` at the top of the file sets the window's size (1: 512x480, 0.5: half). Works in `one` and one-debugger; drawing 512x480 pixels in Lua is slow, so it redraws every few frames (about 20 times a second). one-debugger's `wbg` command is the same view, drawn by the debugger itself (live every frame, and with each line's own CHR banks). |

| `palette.lua` | Palette RAM (BG0-BG3, SP0-SP3) and the 64 NES colors in a window of its own, the ones in use outlined; the line at the bottom says what the mouse is over (`one.windowMouse`). Text is drawn with a 5x7 font kept in the script. Redrawn every frame. `SCALE` at the top sets the window's size. one-debugger's `wpal` command draws the same picture. |

| `pattern.lua` | Both pattern tables side by side in a window of its own; P in the window switches the palette, G a tile grid; the bottom line says which tile the mouse is over. Redrawn every frame (about 2.5 ms). one-debugger's `wpat` command draws the same picture (and also says where a tile is in CHR-ROM, which a script cannot know). |
| `lagcounter.lua` | A lag frame counter (frames in which the game did not read the controllers) drawn at the top left of the game picture, red on a frame that lagged: a memory callback on $4016/$4017, an end-of-frame callback, `emu.drawString`. Only Mesen 2's API, so it also runs in Mesen. ONE has the same counter built in (the menu's "Lag counter", one-debugger's `lag on`); a test checks the script against it. |
| `sprite0.lua` | Sprite 0 on the game picture: a box around it (green if its hit happened this frame, red if not), a line across the picture at its top row with ticks at both edges, and its Y and X as text. Only Mesen 2's API, so it also runs in Mesen. |
| `font5x7.lua` | Not a script to run: the 5x7 font palette.lua and pattern.lua draw their text with (`dofile` from the script's own folder). |

`one.features()` tells a script where it runs (`program`, whether the debugger's functions are there, whether windows
can be seen), so one script can do the right thing in both programs.
