-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- ctest lua_lag / debugger_lua_lag: lua/examples/lagcounter.lua (its path in $EXAMPLE_LUA) counts the lag frames of
-- tests/lag/lag.nes (a poll on three frames in four) the same as ONE's own counter (one.lagCount()), and the draw
-- call does not fail. Checked at startFrame, when both have counted the frame that ended. Prints "lua_lag: PASS" and stops with exit code 0, or prints what failed (exit code 1).
local example = dofile(os.getenv("EXAMPLE_LUA"))
local frame = 0
emu.addEventCallback(function()
  frame = frame + 1
  if frame == 80 then
    local ours, theirs = example.lagCount(), one.lagCount()
    if ours == theirs and ours >= 15 and example.isLagFrame() == one.isLagFrame() then
      print("lua_lag: PASS")
      emu.stop(0)
    else
      print("lua_lag: FAIL script counted " .. ours .. ", ONE " .. theirs .. " (frame lag " ..
            tostring(example.isLagFrame()) .. " / " .. tostring(one.isLagFrame()) .. ")")
      emu.stop(1)
    end
  end
end, emu.eventType.startFrame) -- (ONE's counter takes a frame in at video_update_screen(), just after endFrame)
