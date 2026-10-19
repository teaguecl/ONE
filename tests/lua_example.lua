-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- ctests lua_<example> / debugger_lua_<example>: an example script (lua/examples/$EXAMPLE.lua, its path in
-- $EXAMPLE_LUA) on nestest. After 60 frames it saves its window ($TEST_OUT/lua_$EXAMPLE.png); in one-debugger
-- (one.features().debugger) the debugger's own window of the same kind ($VIEWER: wbg, wpal, wpat) saves its picture
-- of the same moment too ($TEST_OUT/$VIEWER_$EXAMPLE.png), which must be the same picture (tests/png_same.py).
local name, out, viewer = os.getenv("EXAMPLE"), os.getenv("TEST_OUT"), os.getenv("VIEWER")
local example = dofile(os.getenv("EXAMPLE_LUA"))
local frame = 0
emu.addEventCallback(function()
  frame = frame + 1
  if frame == 60 then
    local ok = example.save(out .. "/lua_" .. name .. ".png")
    if one.features().debugger then
      debugger.command(viewer .. " save " .. out .. "/" .. viewer .. "_" .. name .. ".png")
    end
    print(ok and ("lua_" .. name .. ": saved") or ("lua_" .. name .. ": FAIL cannot save"))
    emu.stop(ok and 0 or 1)
  end
end, emu.eventType.endFrame)
