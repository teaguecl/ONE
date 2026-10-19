-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- lagcounter.lua: a lag frame counter on the game picture, written with Mesen 2's Lua API (it also runs in Mesen).
--
--   one --lua lua/examples/lagcounter.lua game.nes       (or in one-debugger: lua lua/examples/lagcounter.lua)
--
-- A lag frame is a frame in which the game did not read the controllers: its main loop took longer than a frame, so
-- the screen showed the previous state again. The script watches reads of $4016 / $4017 (emu.addMemoryCallback) and,
-- at the end of each frame (emu.eventType.endFrame), counts the frame if there was none. "Lag 123" is drawn at the top
-- left of the picture (emu.drawString), in red on a frame that lagged, as FCEUX does.
--
-- The script counts from the moment it is loaded. ONE counts from power-on by itself (one.lagCount(), the menu's "Lag
-- counter" checkbox, one-debugger's lag command): this script is the same thing done in Lua, and the same at
-- the end of a frame (a test compares them). Needs no function of ONE's own.

-- where the text goes (NES pixels), its colors (Mesen's 0xAARRGGBB, alpha inverted: 0x00 is opaque)
local X, Y = 8, 8
local TEXT, LAGGED, BACKGROUND = 0x00FFFFFF, 0x00FF5038, 0x80000000

local count = 0
local polled = false
local lagged = false

-- any read of a controller port is a poll
emu.addMemoryCallback(function() polled = true end, emu.callbackType.read, 0x4016, 0x4017)

emu.addEventCallback(function()
  lagged = not polled
  polled = false
  if lagged then count = count + 1 end
  emu.drawString(X, Y, "Lag " .. count, lagged and LAGGED or TEXT, BACKGROUND)
end, emu.eventType.endFrame)

-- (for the test: the same numbers, to compare with one.lagCount())
return {
  lagCount = function() return count end,
  isLagFrame = function() return lagged end,
}
