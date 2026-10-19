-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- ctest lua_api: the Mesen 2 style Lua API (script.c) on tests/mmc1/mmc1.nes, headless.
-- Prints "lua_api: PASS" and stops with exit code 0, or prints what failed and stops with exit code 1.
local failures = 0
local function check(condition, what)
  if not condition then
    failures = failures + 1
    print("lua_api: FAIL " .. what)
  end
end

local counts = { reset = 0, nmi = 0, startFrame = 0, endFrame = 0, inputPolled = 0 }
for name in pairs(counts) do
  emu.addEventCallback(function() counts[name] = counts[name] + 1 end, emu.eventType[name])
end

-- a callback that removes itself after its first call
local once = 0
local onceRef
onceRef = emu.addEventCallback(function()
  once = once + 1
  emu.removeEventCallback(onceRef, emu.eventType.endFrame)
end, emu.eventType.endFrame)

check(emu.getRomInfo().name == "mmc1.nes", "getRomInfo().name")
check(#emu.getRomInfo().fileCrc32Hash == 8, "getRomInfo().fileCrc32Hash")
check(type(emu.getScriptDataFolder()) == "string", "getScriptDataFolder")

emu.addEventCallback(function()
  local state = emu.getState()
  if counts.endFrame == 1 then
    check(state["ppu.scanline"] == 239 and state["ppu.cycle"] == 338, "endFrame position")
  end
  if counts.endFrame == 10 then
    -- memory: write, read back through two memory types, signed reads, words
    emu.write(0x0300, 0xF0, emu.memType.nesMemory)
    check(emu.read(0x0300, emu.memType.nesInternalRam) == 0xF0, "write/read internal RAM")
    check(emu.read(0x0300, emu.memType.nesMemory, true) == -16, "signed read")
    emu.writeWord(0x0310, 0x1234, emu.memType.nesInternalRam)
    check(emu.readWord(0x0310, emu.memType.nesMemory) == 0x1234, "writeWord/readWord")
    check(emu.read(0x0311, emu.memType.nesMemory) == 0x12, "little-endian word")
    -- the reset vector through the CPU bus is the last bank's, as in the PRG-ROM
    local prgSize = 128 * 1024
    check(emu.readWord(0xFFFC, emu.memType.nesMemory) ==
          emu.readWord(prgSize - 4, emu.memType.nesPrgRom), "PRG-ROM vs CPU bus")
    -- cartridge RAM at $6000 is nesWorkRam offset 0
    emu.write(0x6005, 0x77, emu.memType.nesMemory)
    check(emu.read(5, emu.memType.nesWorkRam) == 0x77, "work RAM")
    -- OAM and palette writes read back
    emu.write(3, 0x42, emu.memType.nesSpriteRam)
    check(emu.read(3, emu.memType.nesSpriteRam) == 0x42, "sprite RAM")
    emu.write(0x11, 0x2A, emu.memType.nesPaletteRam)
    check(emu.read(0x11, emu.memType.nesPaletteRam) == 0x2A, "palette RAM")
    check(emu.read(0x3F11, emu.memType.nesPpuMemory) == 0x2A, "palette through PPU memory")
    check(state["cpu.pc"] >= 0x8000, "cpu.pc")
  end
  if counts.endFrame == 60 then
    -- (one runs the script before the power-on reset; the debugger has already reset the CPU when it starts one)
    check(counts.reset <= 1, "at most one reset event, got " .. counts.reset)
    check(counts.nmi >= 55, "NMI events, got " .. counts.nmi)
    check(counts.startFrame >= 59, "startFrame events, got " .. counts.startFrame)
    check(counts.inputPolled >= 59, "inputPolled events, got " .. counts.inputPolled)
    check(once == 1, "removeEventCallback")
    check(state.frameCount >= 59, "frameCount")
    if failures == 0 then
      print("lua_api: PASS")
      emu.stop(0)
    else
      emu.stop(1)
    end
  end
end, emu.eventType.endFrame)
