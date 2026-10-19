-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- ctest lua_fceux: an FCEUX style script (lua/fceux.lua) on tests/mmc1/mmc1.nes: a main loop with
-- emu.frameadvance(), memory.*, gui.*, joypad.*, savestate.*, rom.*, sound.get(), bit operations and Lua 5.1 names.
-- Prints "lua_fceux: PASS" and exits with code 0, or prints what failed (exit code 1).
local failures = 0
local function check(condition, what)
  if not condition then
    failures = failures + 1
    print("lua_fceux: FAIL " .. what)
  end
end

-- Lua 5.1 names and bit operations
check(unpack ~= nil and loadstring ~= nil and table.getn({ 1, 2 }) == 2, "Lua 5.1 names")
check(string.format("%d", 2.5) == "2" and string.format("%u", 7.9) == "7", "string.format truncates for %d/%u")
check(table.remove({}, 5) == nil, "table.remove past the end")
check(AND(0xF0, 0x3C) == 0x30 and OR(1, 2, 4) == 7 and XOR(0xFF, 0x0F) == 0xF0 and BIT(0, 3) == 9, "AND OR XOR BIT")
check(SHIFT(0x100, 4) == 0x10 and SHIFT(1, -4) == 16 and tobitstring(5) == "00000101", "SHIFT, tobitstring")
check(bit.band(0xFF, 0x0F) == 0x0F and bit.lshift(1, 4) == 16 and bit.tohex(255, 2) == "ff", "bit library")

-- colors
local r, g, b, a = gui.parsecolor("#FF000080")
check(r == 255 and g == 0 and b == 0 and a == 128, "parsecolor #rrggbbaa")
r, g, b, a = gui.parsecolor("green")
check(r == 0 and g == 255 and b == 0 and a == 255, "parsecolor name")
r, g, b, a = gui.parsecolor({ r = 1, g = 2, b = 3 })
check(r == 1 and g == 2 and b == 3 and a == 255, "parsecolor table")

-- the ROM file, iNES header included
check(rom.readbyte(0) == 0x4E and rom.readbyte(3) == 0x1A, "rom.readbyte reads the header")

local writes = 0
-- (writes by the CPU: memory.writebyte does not call it)
memory.registerwrite(0x0000, 0x800, function(address, size, value)
  if address < 0x800 and size == 1 and value >= 0 and value < 256 then writes = writes + 1 end
end)

local before_frame = 0
emu.registerbefore(function() before_frame = before_frame + 1 end)

-- the main loop (it starts before the first frame)
emu.frameadvance()
local start = emu.framecount()
for _ = 1, 5 do emu.frameadvance() end
check(emu.framecount() == start + 5, "frameadvance: 5 frames, got " .. (emu.framecount() - start))
check(before_frame >= 5, "registerbefore: " .. before_frame)

memory.writebyte(0x0400, 0x5A)
check(memory.readbyte(0x0400) == 0x5A, "memory.writebyte / readbyte")
memory.writebyte(0x0401, 0x80)
check(memory.readbytesigned(0x0401) == -128, "readbytesigned")
check(memory.readword(0x0400) == 0x805A, "readword")
check(memory.getregister("pc") >= 0x8000, "getregister")

-- drawing: gui.box fills with the fill color and outlines it opaque; gui.text has a background
gui.box(10, 10, 20, 20, "#FF000080")
gui.pixel(100, 100, "blue")
gui.text(40, 60, "FCEUX", "white", "black")
emu.frameadvance() -- (gui.getpixel: what was drawn for the frame before)
local pr, pg, pb, pa = gui.getpixel(15, 15)
check(pr == 255 and pg == 0 and pb == 0 and pa == 128, string.format("box fill %d %d %d %d", pr, pg, pb, pa))
pr, pg, pb, pa = gui.getpixel(10, 15)
check(pr == 255 and pa == 255, "box outline")
pr, pg, pb, pa = gui.getpixel(100, 100)
check(pb == 255 and pa == 255, "gui.pixel")
pr, pg, pb, pa = gui.getpixel(39, 60)
check(pa == 255 and pr == 0, "gui.text background")

-- joypad.set holds for the next frame only
joypad.set(1, { start = true, A = true })
emu.frameadvance()
local pad = joypad.get(1)
check(pad.start == true and pad.A == true and pad.B == false, "joypad.set / joypad.get")
emu.frameadvance()
check(joypad.get(1).start == false, "joypad.set lasts one frame")

-- save states: taken at the end of the picture, loaded at the next instruction
local state = savestate.object()
memory.writebyte(0x0402, 0x11)
savestate.save(state)
emu.frameadvance()
memory.writebyte(0x0402, 0x22)
savestate.load(state)
emu.frameadvance()
check(memory.readbyte(0x0402) == 0x11, "savestate.load restores RAM: " .. memory.readbyte(0x0402))

local snd = sound.get()
check(snd.rp2a03.square1 ~= nil and type(snd.rp2a03.noise.short) == "boolean" and snd.rp2a03.dpcm.dmcaddress >= 0xC000,
  "sound.get")
check(writes > 100, "memory.registerwrite: " .. writes)
check(type(emu.lagcount()) == "number" and type(emu.lagged()) == "boolean", "lagcount / lagged")

-- a callback cannot call frameadvance
local ok = pcall(emu.frameadvance)
check(ok, "frameadvance in the main loop")
emu.registerafter(function()
  local fine = pcall(emu.frameadvance)
  check(not fine, "frameadvance in a callback raises an error")
  emu.registerafter(nil)
end)
emu.frameadvance()
emu.frameadvance()

if failures == 0 then
  print("lua_fceux: PASS")
  emu.exit()
else
  emu.stop(1)
end
emu.frameadvance()
