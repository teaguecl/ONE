-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- Mesen 2 (Debug > Script Window): prints the CPU cycle of the writes to CHR register 0 ($A000) of tests/nesevent/event.nes and of the first execution of its
-- IRQ handler ($C05A); run the game at maximum speed (Emulation > Speed > Maximum): the IRQ comes 536,870,912 cycles (5 minutes of emulated time) after start 2.
-- ONE (build/linux-debugger/one --headless --unthrottled --lua tests/nesevent/irq_cycle.lua -k 700000000 tests/nesevent/event.nes): "irq handler entered at cycle 536948900".
local function cycles() return emu.getState()["cpu.cycleCount"] end
local writes = 0
emu.addMemoryCallback(function(address, value)
  writes = writes + 1
  if writes <= 20 then emu.log(string.format("write %2d to $A000 (value %02X) at cycle %d", writes, value, cycles())) end
end, emu.callbackType.write, 0xA000, 0xBFFF)
local seen = false
emu.addMemoryCallback(function()
  if not seen then
    seen = true
    emu.log("irq handler entered at cycle " .. cycles())
  end
end, emu.callbackType.exec, 0xC05A)

-- calibration: the cycle an exec callback reports for the first two executions of $C002 (the first STA $A000). ONE: 52 and 63, the same as the trace's
-- Cycle column (the cycle the instruction starts at); if Mesen says 53 and 64 its exec callback counts the opcode fetch and the IRQ numbers agree to the cycle.
local nexec = 0
emu.addMemoryCallback(function()
  nexec = nexec + 1
  if nexec <= 2 then emu.log("exec $C002 at cycle " .. cycles()) end
end, emu.callbackType.exec, 0xC002)
