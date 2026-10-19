-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- ctest lua_zapper: a script aims the Zapper of tests/zapper/zapper_free.nes (which records bits 3 and 4 of every $4017 read of the
-- latest frame at $0200 / $0380: 9 bytes = dark, light, dark) with emu.setInput({x=, y=, trigger=}, 1), as FCEUX's zapper.set()
-- does. Aimed at the white square (136,104): light; at black: none; off the picture with the trigger: bit 4 and no light.
-- emu.getInput(1) gives the table back. Prints "lua_zapper: PASS" and stops with exit code 0, or what failed (exit code 1).
local frame = 0
local failures = {}
local phases = {
  { x = 136, y = 104, trigger = false, light = true,  fire = false, what = "the white square" },
  { x = 20,  y = 20,  trigger = false, light = false, fire = false, what = "black" },
  { x = 136, y = 104, trigger = true,  light = true,  fire = true,  what = "the white square with the trigger" },
  { x = -1,  y = -1,  trigger = true,  light = false, fire = true,  what = "away from the screen with the trigger" },
}
local per_phase = 60

emu.addEventCallback(function()
  local p = phases[frame // per_phase + 1]
  if p ~= nil then
    emu.setInput({ x = p.x, y = p.y, trigger = p.trigger }, 1)
  end
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
  frame = frame + 1
  if frame % per_phase == 0 and frame // per_phase <= #phases then
    local p = phases[frame // per_phase]
    local used = emu.read(0x380, emu.memType.nesDebug)
    local first = emu.read(0x202, emu.memType.nesDebug)
    local got = emu.getInput(1)
    if (used >= 9) ~= p.light or ((first & 0x10) ~= 0) ~= p.fire then
      failures[#failures + 1] = string.format("%s: %d bytes, first read %02X (light %s, trigger %s wanted)", p.what, used, first,
                                              tostring(p.light), tostring(p.fire))
    end
    if got.x ~= p.x or got.y ~= p.y or got.trigger ~= p.trigger then
      failures[#failures + 1] = string.format("%s: getInput gave x=%s y=%s trigger=%s", p.what, tostring(got.x), tostring(got.y),
                                              tostring(got.trigger))
    end
  end
  if frame == per_phase * #phases then
    if #failures == 0 then
      print("lua_zapper: PASS")
      emu.stop(0)
    else
      print("lua_zapper: FAIL " .. table.concat(failures, "; "))
      emu.stop(1)
    end
  end
end, emu.eventType.endFrame)
