-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- ctest debugger_scripting (one-debugger --batch --lua this -s lua_run.txt, on tests/mmc1/mmc1.nes): Lua's
-- debugger.command(), breakpoint conditions (if) and actions (do), source, emu.breakExecution(), emu.getLabelAddress().
-- Prints "debugger_scripting: PASS" and quits with exit code 0, or prints what failed (exit code 1).
local failures = 0
local function check(condition, what)
  if not condition then
    failures = failures + 1
    print("debugger_scripting: FAIL " .. what)
  end
end
local function finish()
  if failures == 0 then print("debugger_scripting: PASS") end
  emu.stop(failures == 0 and 0 or 1)
end

check(debugger ~= nil and type(debugger.command) == "function", "debugger.command exists")

-- a command's output comes back (without the echoed command line)
local out = debugger.command("r a = 5")
check(out == "", "r a = 5 prints nothing, got '" .. out .. "'")
check(emu.getState()["cpu.a"] == 5, "the command ran")
out = debugger.command("bl")
check(out:find("No breakpoints") ~= nil, "bl output: '" .. out .. "'")

-- conditions: compiled when set, shown by bl; a syntax error is reported and sets nothing
check(debugger.command("bpx 8000 if a ==1 +"):find("^Condition:") ~= nil, "syntax error reported")
check(debugger.command("bl"):find("No breakpoints") ~= nil, "nothing set after a syntax error")
debugger.command("bpm 0700 w if value == 0x11 and address == 0x700")
check(debugger.command("bl"):find("BPM W  $0700 if value == 0x11 and address == 0x700", 1, true) ~= nil,
  "bl shows a bpm condition: " .. debugger.command("bl"))
debugger.command("bc *")

-- labels
debugger.command("n c123 my_label")
local label = emu.getLabelAddress("my_label")
check(label ~= nil and label.address == 0xC123, "getLabelAddress")
check(emu.getLabelAddress("no_such_label") == nil, "getLabelAddress of an unknown label")
debugger.command("n c123")

-- source: the file's commands run before what was waiting (the 'x' of the -s file)
local folder = emu.getScriptDataFolder()
local file = folder .. "/source_test.txt"
local f = io.open(file, "w")
f:write("# a breakpoint at the NMI handler's RTI, in frame 3 only\n", "bop 40 if frame == 3\n")
f:close()
debugger.command("source " .. file)

-- (lua itself cannot be used from a script: see the ctest debugger_lua_files for file names)
check(debugger.command("lua x.lua"):find("inside a Lua callback", 1, true) ~= nil, "lua refused inside a script")

-- the stops (codeBreak happens outside the emulation: commands that run the game are allowed there)
local stops, refused = 0, nil
emu.addEventCallback(function()
  if refused == nil then refused = debugger.command("t") end -- (inside the emulation: refused)
end, emu.eventType.endFrame)

emu.addEventCallback(function()
  stops = stops + 1
  local st = emu.getState()
  if stops == 1 then
    check(debugger.command("bl"):find("BOP    opcode $40 if frame == 3", 1, true) ~= nil, "source set the breakpoint")
    check(st["ppu.frameCount"] == 3, "condition: stopped in frame 3, got " .. st["ppu.frameCount"])
    check(emu.read(st["cpu.pc"]) == 0x40, "stopped at the RTI")
    check(refused ~= nil and refused:find("Cannot run the game") ~= nil, "t refused inside a callback: " ..
      tostring(refused))
    -- an action: every RTI stops, and the action goes on
    debugger.command("bc *")
    debugger.command('bop 40 do "x"')
    debugger.command("x")
  elseif stops < 6 then
    check(emu.read(st["cpu.pc"]) == 0x40, "action stop " .. stops .. " at the RTI")
  elseif stops == 6 then
    -- emu.breakExecution(): from inside the emulation, a stop at the next instruction
    debugger.command("bc *")
    local ref
    ref = emu.addEventCallback(function()
      emu.removeEventCallback(ref, emu.eventType.startFrame)
      emu.breakExecution()
    end, emu.eventType.startFrame)
  elseif stops == 7 then
    check(st["ppu.scanline"] == -1, "breakExecution stopped in the pre-render line, got " .. st["ppu.scanline"])
    finish()
  end
end, emu.eventType.codeBreak)
