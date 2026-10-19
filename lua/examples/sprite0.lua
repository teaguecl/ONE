-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- sprite0.lua: shows where sprite 0 is on the game picture, written with Mesen 2's Lua API (it also runs in Mesen).
--
--   one --lua lua/examples/sprite0.lua game.nes       (or in one-debugger: lua lua/examples/sprite0.lua)
--
-- Sprite 0 is the first sprite in OAM (bytes 0-3: Y, tile, attributes, X). Games use it for split screens: the PPU
-- sets bit 6 of $2002 when an opaque pixel of sprite 0 overlaps an opaque background pixel, and the game waits for
-- that to change the scroll in the middle of the frame. The script draws, every frame, at the end of the picture:
--   - a box around sprite 0 (8x8 or 8x16 as PPUCTRL says), green if the hit happened this frame, red if not;
--   - a line across the whole picture at the sprite's top row, the first line where the hit can happen
--     (the sprite's Y byte + 1), with a tick at both edges, and the Y and X as text.
-- Set LINE to false to leave out the line across the picture.

local LINE = true
local HIT, MISS, TEXT, BACKGROUND = 0x0000FF00, 0x00FF3030, 0x00FFFFFF, 0x80000000
local LINE_ALPHA = 0x60 -- the line is faint (Mesen's inverted alpha: 0x00 opaque, 0xFF invisible)

local function oam(i) return emu.read(i, emu.memType.nesSpriteRam) end

emu.addEventCallback(function()
  local state = emu.getState()
  local y, x = oam(0) + 1, oam(3)
  local height = (state["ppu.control"] & 0x20) ~= 0 and 16 or 8
  local hit = (state["ppu.status"] & 0x40) ~= 0 -- still set at the end of the picture (it clears at the pre-render line)
  local color = hit and HIT or MISS

  if y >= 240 then -- off the picture: no hit possible
    emu.drawString(2, 2, string.format("sprite 0 hidden (Y=$%02X)", y - 1), TEXT, BACKGROUND)
    return
  end

  if LINE then
    emu.drawLine(0, y, 255, y, (LINE_ALPHA << 24) | (color & 0xFFFFFF))
  end
  emu.drawLine(0, y, 5, y, color) -- ticks at both edges
  emu.drawLine(250, y, 255, y, color)
  emu.drawRectangle(x, y, 8, height, color, false)

  local text = string.format("S0 y=%d x=%d %s", y, x, hit and "hit" or "no hit")
  local ty = (y > 20) and (y - 10) or (y + height + 2)
  emu.drawString(math.min(x, 256 - 6 * #text), ty, text, color, BACKGROUND)
end, emu.eventType.endFrame)
