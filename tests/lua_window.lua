-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- ctest lua_window: ONE's own windows for scripts (one.openWindow and the rest, script_window.c) and
-- one.features(), in 'one' and in one-debugger -- both headless here, so the windows exist as pictures only. Prints
-- "lua_window: PASS" and stops with exit code 0, or prints what failed (exit code 1).
local failures = 0
local function check(condition, what)
  if not condition then
    failures = failures + 1
    print("lua_window: FAIL " .. what)
  end
end

-- one.features(): which program, whether the debugger's functions are there, whether windows can be seen
local f = one.features()
check(f.program == "one" or f.program == "one-debugger", "features().program: " .. tostring(f.program))
check(f.debugger == (debugger ~= nil and debugger.command ~= nil), "features().debugger matches debugger.command")
check(f.debugger == (emu.breakExecution ~= nil), "features().debugger matches emu.breakExecution")
check(f.program ~= "one" or not f.debugger, "one has no debugger")
check(f.windows == false, "headless: windows are not shown")

-- windows: open, set the picture, save it, keys, close; errors
local a = one.openWindow("lua_window a", 4, 3)
local b = one.openWindow("lua_window b", 2, 2)
check(type(a) == "number" and type(b) == "number" and a ~= b, "openWindow ids: " .. tostring(a) .. " " .. tostring(b))
local none, why = one.openWindow("too small", 0, 5)
check(none == nil and type(why) == "string", "openWindow 0 wide: nil and why")
local half = one.openWindow("lua_window half", 8, 8, 0.5)
check(type(half) == "number", "openWindow with a scale")
one.closeWindow(half)
local bad, why2 = one.openWindow("bad scale", 8, 8, -1)
check(bad == nil and type(why2) == "string", "openWindow with a negative scale: nil and why")

local pixels = {}
for i = 1, 12 do pixels[i] = i * 0x111111 end
check(pcall(one.setWindowPixels, a, pixels), "setWindowPixels")
check(not pcall(one.setWindowPixels, a, {1, 2, 3}), "setWindowPixels with too few pixels fails")

local file = "lua_window.png"
check(one.saveWindow(a, file) == true, "saveWindow")
local png = io.open(file, "rb")
check(png ~= nil and png:read(8) == "\x89PNG\r\n\x1a\n", "saveWindow wrote a PNG")
if png then png:close() end

local keys = one.windowKeys(a)
check(type(keys) == "table" and #keys == 0, "windowKeys: none pressed")
check(one.windowMouse(a) == nil, "windowMouse: not over it")

one.closeWindow(b)
check(not pcall(one.windowKeys, b), "a closed window is gone")
check(not pcall(one.setWindowPixels, 99, pixels), "not a window of this script")

if failures == 0 then
  print("lua_window: PASS")
  emu.stop(0)
else
  emu.stop(1)
end
