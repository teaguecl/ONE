-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- pattern.lua: a pattern table viewer in a window of its own, written with ONE's Lua API (after Mesen's Tile Viewer).
--
--   one --lua lua/examples/pattern.lua game.nes          (or in one-debugger: lua lua/examples/pattern.lua)
--
-- The window shows both pattern tables side by side ($0000 left, $1000 right, a line between them), 16 x 16 tiles
-- each, through the cartridge's CHR banks as they are at the end of each frame. Keys in the window: P switches the
-- palette (BG0-BG3, SP0-SP3, gray; BG0 to start with), G lays a tile grid over the tiles. The labels say which table
-- PPUCTRL uses for the background (BG) and for sprites (SP); the line at the bottom says which tile the mouse is
-- over. one-debugger's wpat command draws the same picture (a test compares the two; wpat's mouse line also says
-- where the tile is in CHR-ROM, which a script cannot know).
--
-- Uses: one.openWindow, one.setWindowPixels, one.windowKeys, one.windowMouse, one.paletteColor (ONE's own);
-- emu.read with nesPpuMemory and nesPaletteRam, emu.getState, emu.addEventCallback (Mesen 2's API); font5x7.lua.

-- How large the window is: SCALE times the 273 x 157 picture (1, 1.5, 2...; nil: twice if that fits on the screen)
local SCALE = 2

local W, H = 273, 157
local window, why = one.openWindow("Pattern tables - pattern.lua (P: palette, G: grid)", W, H, SCALE)
if not window then
  emu.log("pattern.lua: " .. tostring(why))
  return
end

local folder = debug.getinfo(1, "S").source:match("^@(.*[/\\])") or ""
local font = dofile(folder .. "font5x7.lua")

-- the layout (the same numbers as one-debugger's pattern viewer, debugger/pattern_viewer.cpp)
local BACKGROUND, LABEL, SEPARATOR = 0x202020, 0xC0C0C0, 0x606060
local TABLE_LEFT, TABLE_TOP, SEPARATOR_X, LABEL_TOP, INFO_TOP = { [0] = 4, [1] = 141 }, 14, 136, 4, 146

local pixels = {}
local palette, grid = 0, false

local function fill(x, y, w, h, color)
  for row = y, y + h - 1 do
    local base = row * W + 1
    for col = x, x + w - 1 do pixels[base + col] = color end
  end
end

local function readPpu(address) return emu.read(address, emu.memType.nesPpuMemory) end

-- the tile under the mouse: its number and PPU address; "" when the mouse is over neither table
local function hover(mx, my)
  if not mx then return "" end
  for t = 0, 1 do
    local x, y = mx - TABLE_LEFT[t], my - TABLE_TOP
    if x >= 0 and x < 128 and y >= 0 and y < 128 then
      local tile = (y // 8) * 16 + x // 8
      return string.format("TILE $%02X  $%04X", tile, t * 0x1000 + tile * 16)
    end
  end
  return ""
end

local function draw()
  fill(0, 0, W, H, BACKGROUND)
  -- the four colors: palette RAM's (color 0: the backdrop, as the PPU draws it), or grays
  local colors = { [0] = 0x000000, 0x555555, 0xAAAAAA, 0xFFFFFF }
  if palette < 8 then
    for i = 0, 3 do
      colors[i] = one.paletteColor(emu.read(i == 0 and 0 or palette * 4 + i, emu.memType.nesPaletteRam) & 0x3F)
    end
  end
  local ctrl = emu.getState()["ppu.control"]
  for t = 0, 1 do
    -- the label: which table this is, and what PPUCTRL uses it for (8x16 sprites use both)
    local label = t == 0 and "$0000" or "$1000"
    if (ctrl >> 4) & 1 == t then label = label .. " BG" end
    if ctrl & 0x20 ~= 0 or (ctrl >> 3) & 1 == t then label = label .. " SP" end
    font.text(pixels, W, H, TABLE_LEFT[t], LABEL_TOP, label, LABEL)
    for tile = 0, 255 do
      local left, top = TABLE_LEFT[t] + (tile % 16) * 8, TABLE_TOP + (tile // 16) * 8
      for row = 0, 7 do
        local address = t * 0x1000 + tile * 16 + row
        local low, high = readPpu(address), readPpu(address + 8)
        local out = (top + row) * W + left + 1
        for x = 0, 7 do
          local bit = 7 - x
          local color = colors[((low >> bit) & 1) | (((high >> bit) & 1) << 1)]
          if grid and (x == 0 or row == 0) then
            color = ((color >> 1) & 0x7F7F7F) + 0x404040 -- (the grid: lightened)
          end
          pixels[out + x] = color
        end
      end
    end
  end
  fill(SEPARATOR_X, LABEL_TOP, 1, TABLE_TOP + 128 - LABEL_TOP, SEPARATOR)
  font.text(pixels, W, H, 4, INFO_TOP, hover(one.windowMouse(window)), LABEL)
  one.setWindowPixels(window, pixels)
end

emu.addEventCallback(function()
  local keys = one.windowKeys(window)
  if keys == nil then return end -- (the user closed the window)
  for _, key in ipairs(keys) do
    if key == "P" then palette = (palette + 1) % 9 end
    if key == "G" then grid = not grid end
  end
  draw()
end, emu.eventType.endFrame)
draw()

-- for tests: save what the window shows
return { save = function(file) draw() return one.saveWindow(window, file) end }
