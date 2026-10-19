-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- ctest lua_api2: drawing, input, memory callbacks and save states of the Mesen 2 style Lua API (script.c), on
-- tests/mmc1/mmc1.nes. Prints "lua_api2: PASS" and stops with exit code 0, or prints what failed (exit code 1).
local failures = 0
local function check(condition, what)
  if not condition then
    failures = failures + 1
    print("lua_api2: FAIL " .. what)
  end
end
local function hex(v) return string.format("%08X", v) end

-- memory callbacks: writes to internal RAM, the NMI handler's first instruction, reads of $2002 (PPU status), and
-- a callback that is removed again
local writes, execs, reads, removed_calls = 0, 0, 0, 0
local nmi = emu.readWord(0xFFFA, emu.memType.nesMemory)
emu.addMemoryCallback(function() writes = writes + 1 end, emu.callbackType.write, 0x0000, 0x07FF)
emu.addMemoryCallback(function() execs = execs + 1 end, emu.callbackType.exec, nmi)
emu.addMemoryCallback(function(address, value)
  if address == 0x2002 and (value & 0x1F) == 0 then reads = reads + 1 end -- (no open bus in bits 0-4 here)
end, emu.callbackType.read, 0x2002)
local removed = emu.addMemoryCallback(function() removed_calls = removed_calls + 1 end, emu.callbackType.write,
  0x0000, 0x07FF)
emu.removeMemoryCallback(removed, emu.callbackType.write, 0x0000, 0x07FF)

local frame, state, loaded_events = 0, nil, 0
emu.addEventCallback(function() loaded_events = loaded_events + 1 end, emu.eventType.stateLoaded)
emu.addEventCallback(function() if frame >= 3 and frame < 8 then emu.setInput({ start = true }, 0) end end,
  emu.eventType.inputPolled)

emu.addEventCallback(function()
  frame = frame + 1
  local fc = emu.getState().frameCount
  if frame == 5 then
    -- drawings: Mesen colors (alpha inverted: 0 = opaque); one.hudPixel reads the layer (ARGB, 255 = opaque)
    -- the frame after
    emu.drawRectangle(10, 10, 20, 10, 0x00FF0000, true)
    emu.drawPixel(100, 100, 0x8000FF00, 3)
    emu.drawLine(0, 200, 255, 200, 0x00FFFFFF)
    emu.drawString(50, 50, "Hi!", 0x00FFFFFF, 0x00000000)
    check(emu.getInput(0).start == true, "getInput sees setInput")
    check(emu.getInput(0).a == false, "getInput: buttons not set are up")
  elseif frame == 6 then
    check(one.hudPixel(15, 15) == 0xFFFF0000, "filled rectangle " .. hex(one.hudPixel(15, 15)))
    check((one.hudPixel(100, 100) >> 24) == 0x7F and (one.hudPixel(100, 100) & 0xFFFFFF) == 0x00FF00,
      "half-transparent pixel " .. hex(one.hudPixel(100, 100)))
    check(one.hudPixel(128, 200) == 0xFFFFFFFF, "line " .. hex(one.hudPixel(128, 200)))
    check(one.hudPixel(50, 50) == 0xFF000000, "string background " .. hex(one.hudPixel(50, 50)))
    check(one.hudPixel(200, 100) == 0, "untouched pixel " .. hex(one.hudPixel(200, 100)))
  elseif frame == 9 then
    -- the 1-frame drawings are gone, the 3-frame pixel too
    check(one.hudPixel(15, 15) == 0 and one.hudPixel(100, 100) == 0, "drawings expire")
    check(emu.getInput(0).start == false, "setInput lasts one frame")
  elseif frame == 10 then
    emu.write(0x0400, 0x11)
    state = emu.createSavestate()
    check(state ~= nil, "createSavestate")
  elseif frame == 11 then
    check(one.stateReady(state), "the state is taken by the next frame")
    emu.write(0x0400, 0x22)
  elseif frame == 20 then
    emu.loadSavestate(state)
  elseif frame == 21 then
    check(fc == 11 or fc == 10, "frame count after loading: " .. fc)
    check(emu.read(0x0400) == 0x11, "RAM after loading: " .. emu.read(0x0400))
    check(loaded_events == 1, "stateLoaded events: " .. loaded_events)
  elseif frame == 60 then
    check(writes > 100, "write callbacks: " .. writes)
    check(execs >= 40, "exec callbacks (NMI handler): " .. execs)
    check(reads >= 1, "read callbacks: " .. reads)
    check(removed_calls == 0, "removeMemoryCallback")
    check(type(one.lagCount()) == "number", "lagCount")
    if failures == 0 then
      print("lua_api2: PASS")
      emu.stop(0)
    else
      emu.stop(1)
    end
  end
end, emu.eventType.endFrame)
