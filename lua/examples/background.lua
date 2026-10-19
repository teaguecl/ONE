-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- background.lua: a background (nametable) viewer in a window of its own, written with ONE's Lua API.
--
--   one --lua lua/examples/background.lua game.nes          (or in one-debugger: lua lua/examples/background.lua)
--
-- The window shows the four nametables (512 x 480: $2000 $2400 / $2800 $2C00, through the cartridge's mirroring),
-- drawn with the background pattern table and palettes as they are at the end of each frame, and the part the game
-- window shows at full brightness with a white outline; the rest is dimmed. O in the window switches the outline
-- between "each line" (every picture line where its own scroll put it: status bars, split screens and parallax
-- show as separate pieces) and "one rectangle" (256 x 240 from the scroll the picture starts with).
--
-- How it knows each line's scroll: it watches the game's writes to the PPU's registers (emu.addMemoryCallback) and
-- keeps its own copy of the PPU's scroll registers ("loopy" v, t, x), applying them line by line as the PPU does:
-- at dot 256 of a line the vertical position moves down a row, at dot 257 the horizontal position is copied from t,
-- on the pre-render line dots 280-304 the vertical one too, and at dot 321 the next line's first tiles are fetched
-- with v as it is then. (one-debugger's own viewer, the wbg command, reads the same thing from the PPU itself.)
--
-- Uses: one.openWindow, one.setWindowPixels, one.windowKeys, one.features (ONE's own); emu.addMemoryCallback,
-- emu.addEventCallback, emu.getState, emu.read (Mesen 2's API).

-- How large the window is: SCALE times the 512 x 480 picture (0.5: 256 x 240, 1: 512 x 480, 1.5, 2...; nil: twice if
-- that fits on the screen). Edit it to taste; the window can also be resized with the mouse.
local SCALE = 1

local W, H = 512, 480
local features = one.features()
local window, why = one.openWindow("Background - background.lua (O: overlay)", W, H, SCALE)
if not window then
  emu.log("background.lua: " .. tostring(why))
  return
end
if not features.windows then
  emu.log("background.lua: no windows can be shown here (headless); one.saveWindow() still works")
end

-- --- the scroll, line by line ---------------------------------------------------------------------------------------

-- the PPU's registers as the game set them (to start with: as they are now)
local now = emu.getState()
local t, fineX, v = now["ppu.tmpVideoRamAddr"], now["ppu.xScroll"], now["ppu.videoRamAddr"]
local mask, ctrl = now["ppu.mask"], now["ppu.control"]
local lines = {}                              -- lines[0..239] = {v =, x =, mask =} as each picture line starts
for i = 0, 239 do lines[i] = { v = 0, x = 0, mask = 0 } end
local curLine, curStep = 239, 1               -- the next PPU step to apply (239: none until the next frame)
local STEPS = { 256, 257, 304, 321 }          -- (304: the pre-render line's vertical copy only)

local function rendering() return (mask & 0x18) ~= 0 end

local function incrementY()                   -- dot 256: down one pixel row (fine Y, then coarse Y, then nametable)
  if (v & 0x7000) ~= 0x7000 then
    v = v + 0x1000
  else
    v = v & ~0x7000
    local y = (v & 0x03E0) >> 5
    if y == 29 then
      y = 0
      v = v ~ 0x0800
    elseif y == 31 then
      y = 0
    else
      y = y + 1
    end
    v = (v & ~0x03E0) | (y << 5)
  end
end

local function step(dot)
  if dot == 256 then
    if rendering() then incrementY() end
  elseif dot == 257 then
    if rendering() then v = (v & ~0x041F) | (t & 0x041F) end
  elseif dot == 304 then
    if curLine == -1 and rendering() then v = (v & ~0x7BE0) | (t & 0x7BE0) end
  else -- 321: the next line starts with this
    local l = lines[curLine + 1]
    l.v, l.x, l.mask, l.ctrl = v, fineX, mask, ctrl
  end
end

-- apply the PPU's steps up to scanline SL (-1: pre-render), dot DOT
local function advance(sl, dot)
  while curLine <= 238 and (curLine < sl or (curLine == sl and STEPS[curStep] <= dot)) do
    step(STEPS[curStep])
    curStep = curStep + 1
    if curStep > #STEPS then
      curLine, curStep = curLine + 1, 1
    end
  end
end

-- the game writes a PPU register ($2000-$3FFF: eight registers, mirrored)
emu.addMemoryCallback(function(address, value)
  local reg = address & 7
  if reg ~= 0 and reg ~= 1 and reg ~= 5 and reg ~= 6 then
    return -- ($2007 data writes: many, and the scroll does not care)
  end
  local s = emu.getState()
  local sl = s["ppu.scanline"]
  advance(sl == -1 and -1 or sl, s["ppu.cycle"])
  local w = s["ppu.writeToggle"]
  local second = (w == true or w == 1) -- (the toggle as it was before this write)
  if reg == 0 then
    ctrl = value
    t = (t & ~0x0C00) | ((value & 0x03) << 10)
  elseif reg == 1 then
    mask = value
  elseif reg == 5 then
    if not second then
      t = (t & ~0x001F) | (value >> 3)
      fineX = value & 7
    else
      t = (t & ~0x73E0) | ((value & 0x07) << 12) | ((value & 0xF8) << 2)
    end
  else -- reg 6
    if not second then
      t = (t & 0x00FF) | ((value & 0x3F) << 8)
    else
      t = (t & 0xFF00) | value
      v = t
    end
  end
end, emu.callbackType.write, 0x2000, 0x3FFF)

-- --- drawing ----------------------------------------------------------------------------------------------------------

local pixels = {}
for i = 1, W * H do pixels[i] = 0 end
local perLine = true
local skip = 0 -- frames to let pass before drawing again (drawing is slow: see the endFrame callback)

local function readPpu(address) return emu.read(address, emu.memType.nesPpuMemory) end

local function draw()
  -- the palettes: color 0 of each is the backdrop; dimmed copies for what the game window does not show
  local colors, dim = {}, {}
  local backdrop = one.paletteColor(emu.read(0, emu.memType.nesPaletteRam) & 0x3F)
  for i = 0, 31 do
    colors[i] = (i & 3) == 0 and backdrop or one.paletteColor(emu.read(i, emu.memType.nesPaletteRam) & 0x3F)
    dim[i] = (colors[i] >> 1) & 0x7F7F7F
  end

  -- which pixels the game window shows: shown[y][x] (rows that show nothing are absent)
  local shown = {}
  local function show(y, x0, from)
    local row = shown[y]
    if not row then row = {}; shown[y] = row end
    for i = from, 255 do row[(x0 + i) % W] = true end
  end
  for line = 0, 239 do
    local l = lines[line]
    local coarseY = (l.v >> 5) & 0x1F
    if (l.mask & 0x08) ~= 0 and coarseY < 30 then
      local x0 = ((l.v >> 10) & 1) * 256 + (l.v & 0x1F) * 8 + l.x
      local y = ((l.v >> 11) & 1) * 240 + coarseY * 8 + ((l.v >> 12) & 7)
      if perLine then
        show(y, x0, (l.mask & 0x02) ~= 0 and 0 or 8)
      else -- one rectangle, from the first line with background
        for row = 0, 239 do show((y - line + row) % H, x0, 0) end
        break
      end
    end
  end

  -- the four nametables (read once: 4 x 1KB), then drawn a pixel row at a time; tiles' rows decoded once (cache)
  local patterns = (ctrl & 0x10) ~= 0 and 0x1000 or 0x0000
  local nametables = {}
  for i = 0, 4095 do nametables[i] = readPpu(0x2000 + i) end
  local cache = {}
  for y = 0, H - 1 do
    local base = (y // 240) * 2 -- (the nametable row: $2000/$2400 or $2800/$2C00)
    local ty, fineY = (y % 240) // 8, y % 8
    local row = shown[y]
    local out = y * W + 1
    for column = 0, 63 do
      local nt = (base + column // 32) * 0x400
      local tx = column % 32
      local tile = nametables[nt + ty * 32 + tx]
      local attribute = nametables[nt + 0x3C0 + (ty // 4) * 8 + tx // 4]
      local palette = ((attribute >> (((ty & 2) << 1) | (tx & 2))) & 3) * 4
      local key = (tile * 8 + fineY) * 4 + palette // 4
      local bits = cache[key]
      if not bits then
        local address = patterns + tile * 16 + fineY
        local low, high = readPpu(address), readPpu(address + 8)
        bits = {}
        for x = 0, 7 do
          local b = 7 - x
          local c = ((low >> b) & 1) | (((high >> b) & 1) << 1)
          bits[x] = c == 0 and 0 or palette + c
        end
        cache[key] = bits
      end
      local px = column * 8
      if row then
        for x = 0, 7 do
          local c = bits[x]
          pixels[out + px + x] = row[px + x] and colors[c] or dim[c]
        end
      else -- (a row the game window does not show: all dimmed)
        for x = 0, 7 do pixels[out + px + x] = dim[bits[x]] end
      end
    end
  end

  -- the white outline: shown pixels next to one that is not (the map wraps around: so does the part shown)
  for y, row in pairs(shown) do
    local above, below = shown[(y - 1) % H], shown[(y + 1) % H]
    for x in pairs(row) do
      if not (row[(x - 1) % W] and row[(x + 1) % W] and above and above[x] and below and below[x]) then
        pixels[y * W + x + 1] = 0xFFFFFF
      end
    end
  end
  one.setWindowPixels(window, pixels)
end

-- --- events -----------------------------------------------------------------------------------------------------------

emu.addEventCallback(function()
  curLine, curStep = -1, 1 -- the pre-render line begins: the steps start again
end, emu.eventType.startFrame)

emu.addEventCallback(function()
  advance(239, 0) -- (line 239 was set up at dot 321 of line 238)
  local keys = one.windowKeys(window)
  if keys == nil then
    return -- (the window was closed)
  end
  for _, key in ipairs(keys) do
    if key == "O" then
      perLine = not perLine
      skip = 0 -- (show the change now)
    end
  end
  -- Drawing 512 x 480 pixels in Lua takes a while: skip frames so that it uses at most about a third of the time
  -- (a frame is 1/60 s); when the game is stopped in the debugger, every frame it steps is drawn.
  if skip > 0 then
    skip = skip - 1
    return
  end
  local start = os.clock()
  draw()
  skip = math.min(10, math.floor((os.clock() - start) / (0.33 / 60)))
end, emu.eventType.endFrame)

-- for tests: save what the window shows
return { save = function(file) draw() return one.saveWindow(window, file) end } -- (drawn now: frames are skipped)
