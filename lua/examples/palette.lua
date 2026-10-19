-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- palette.lua: a palette viewer in a window of its own, written with ONE's Lua API (after Mesen's Palette Viewer).
--
--   one --lua lua/examples/palette.lua game.nes          (or in one-debugger: lua lua/examples/palette.lua)
--
-- The window shows palette RAM -- BG0-BG3 and SP0-SP3, four colors each, with their values ($3F10 $3F14 $3F18 $3F1C,
-- which are the same bytes as $3F00 $3F04 $3F08 $3F0C, marked in a corner) -- and the NES's 64 colors, the ones
-- palette RAM uses outlined. The line at the bottom says what the mouse is over. It is redrawn every frame (it is
-- small). one-debugger's wpal command draws the same picture (a test compares the two).
--
-- Uses: one.openWindow, one.setWindowPixels, one.windowMouse, one.paletteColor (ONE's own); emu.read with
-- emu.memType.nesPaletteRam, emu.addEventCallback (Mesen 2's API). Text is drawn with font5x7.lua (next to it: the
-- 5x7 font ONE's game window uses for its messages).

-- How large the window is: SCALE times the 280 x 218 picture (1, 1.5, 2...; nil: twice if that fits on the screen)
local SCALE = 1

local W, H = 280, 218
local window, why = one.openWindow("Palette - palette.lua", W, H, SCALE)
if not window then
  emu.log("palette.lua: " .. tostring(why))
  return
end

-- the layout (the same numbers as one-debugger's palette viewer, debugger/palette_viewer.cpp)
local BACKGROUND, LABEL = 0x202020, 0xC0C0C0
local RAM_TOP, RAM_ROW_PITCH, SWATCH_LEFT, SWATCH_PITCH, SWATCH_W, SWATCH_H = 4, 18, 26, 26, 24, 16
local GRID_LEFT, GRID_TOP, CELL_PITCH_X, CELL_PITCH_Y, CELL_W, CELL_H = 4, 152, 17, 13, 16, 12
local INFO_TOP = 207

-- the 5x7 font, from font5x7.lua next to this script
local folder = debug.getinfo(1, "S").source:match("^@(.*[/\\])") or ""
local font = dofile(folder .. "font5x7.lua")

local pixels = {}

local function fill(x, y, w, h, color)
  for row = y, y + h - 1 do
    local base = row * W + 1
    for col = x, x + w - 1 do pixels[base + col] = color end
  end
end

local function text(x, y, s, color) font.text(pixels, W, H, x, y, s, color) end

local function contrast(color) -- black or white, whichever reads better on COLOR
  local r, g, b = (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF
  return (r * 299 + g * 587 + b * 114) // 1000 >= 128 and 0x000000 or 0xFFFFFF
end

local function entry(i) return emu.read(i, emu.memType.nesPaletteRam) & 0x3F end

-- what the mouse is over: a palette RAM entry or one of the 64 colors; "" when nothing
local function hover(mx, my)
  if not mx then return "" end
  for e = 0, 31 do
    local x, y = SWATCH_LEFT + (e & 3) * SWATCH_PITCH, RAM_TOP + (e >> 2) * RAM_ROW_PITCH
    if mx >= x and mx < x + SWATCH_W and my >= y and my < y + SWATCH_H then
      local value = entry(e)
      return string.format("$3F%02X %s%d:%d = $%02X #%06X", e, e < 16 and "BG" or "SP", (e >> 2) & 3, e & 3, value,
        one.paletteColor(value))
    end
  end
  for c = 0, 63 do
    local x, y = GRID_LEFT + (c & 15) * CELL_PITCH_X, GRID_TOP + (c >> 4) * CELL_PITCH_Y
    if mx >= x and mx < x + CELL_W and my >= y and my < y + CELL_H then
      return string.format("COLOR $%02X #%06X", c, one.paletteColor(c))
    end
  end
  return ""
end

local function draw()
  fill(0, 0, W, H, BACKGROUND)
  local used = {}
  for e = 0, 31 do -- palette RAM: a row per palette
    local row, i = e >> 2, e & 3
    local x, y = SWATCH_LEFT + i * SWATCH_PITCH, RAM_TOP + row * RAM_ROW_PITCH
    if i == 0 then text(4, y + 4, (row < 4 and "BG" or "SP") .. (row & 3), LABEL) end
    local value = entry(e)
    used[value] = true
    local color = one.paletteColor(value)
    local ink = contrast(color)
    fill(x, y, SWATCH_W, SWATCH_H, color)
    text(x + 3, y + 4, string.format("$%02X", value), ink)
    if e >= 16 and i == 0 then -- ($3F10 $3F14 $3F18 $3F1C: the same bytes as $3F00 $3F04 $3F08 $3F0C)
      fill(x + 1, y + 1, 3, 1, ink)
      fill(x + 1, y + 1, 1, 3, ink)
    end
  end
  for c = 0, 63 do -- the 64 colors, 16 a row; the ones palette RAM uses outlined
    local x, y = GRID_LEFT + (c & 15) * CELL_PITCH_X, GRID_TOP + (c >> 4) * CELL_PITCH_Y
    local color = one.paletteColor(c)
    local ink = contrast(color)
    fill(x, y, CELL_W, CELL_H, color)
    text(x + 2, y + 2, string.format("%02X", c), ink)
    if used[c] then
      fill(x, y, CELL_W, 1, ink)
      fill(x, y + CELL_H - 1, CELL_W, 1, ink)
      fill(x, y, 1, CELL_H, ink)
      fill(x + CELL_W - 1, y, 1, CELL_H, ink)
    end
  end
  text(4, INFO_TOP, hover(one.windowMouse(window)), LABEL)
  one.setWindowPixels(window, pixels)
end

emu.addEventCallback(function()
  if one.windowKeys(window) ~= nil then draw() end -- (nil: the user closed the window)
end, emu.eventType.endFrame)
draw()

-- for tests: save what the window shows
return { save = function(file) draw() return one.saveWindow(window, file) end }
