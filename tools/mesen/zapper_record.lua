-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- zapper_record.lua: records where Mesen's Zapper (port 2) is aimed, as an input script for `one --input FILE`
-- ("CYCLE zapper X Y [fire]" lines). emu.setInput does not move the Zapper (it keeps
-- following the mouse), so the aim is made by hand and recorded; ONE then replays what Mesen saw.
--
-- Use with tests/zapper/zapper_free.nes, which measures every frame, forever (no stepping, no exact aim needed):
--   1. Settings -> Input: port 2 = Zapper (NES). In the Script Window open Settings, tick "Allow access to I/O and
--      OS functions", load and run this script.
--   2. Load zapper_free.nes. Pause (Esc), Game -> Power Cycle, open the Trace Logger and start file logging.
--   3. Resume, and for 2-3 seconds move the mouse across the white square (it is in the middle of the picture, a bit
--      above the center) and over its edges, in slow sweeps; hold the left button for a moment while on it, and
--      once while off it. Then pause and stop logging. (A frame is about 1 MB of trace: 150 frames, 150 MB.)
--   4. The script wrote zapper_input_mesen.txt (next to this script): the aim as the ROM saw it. Run ONE with
--      `--input` on it and compare the $4017 reads with the trace.
-- Positions outside the picture are written as "off". The cycle is Mesen's CPU cycle count at the start of the frame.

local PORT = nil               -- the port with the Zapper (x, y, trigger), found at the first frame; set 0-3 to force one
local OUT_NAME = nil           -- set a full path to write somewhere else

local folder = ""
local src = debug and debug.getinfo and debug.getinfo(1, "S")
if src and src.source and string.sub(src.source, 1, 1) == "@" then
  folder = string.match(string.sub(src.source, 2), "^(.*[/\\])") or ""
end
local path = OUT_NAME or (folder .. "zapper_input_mesen.txt")
if io == nil or io.open == nil then
  emu.log("zapper_record.lua: file access is off. In the Script Window, open Settings and tick")
  emu.log("'Allow access to I/O and OS functions', then run the script again.")
  return
end
local file, err = io.open(path, "w")
if file == nil then
  emu.log("zapper_record.lua: cannot write " .. path .. ": " .. tostring(err))
  return
end
file:write("# Zapper aim as Mesen saw it (tools/mesen/zapper_record.lua): CYCLE zapper X Y [fire]\n")
file:flush()

local last = ""
local lines = 0

local function aim_text(x, y, trigger)
  local text
  if x == nil or y == nil or x < 0 or x > 255 or y < 0 or y > 239 then
    text = "off"
  else
    text = x .. " " .. y
  end
  return text .. (trigger and " fire" or "")
end

local function describe(port)
  local ok, input = pcall(emu.getInput, port, 0)
  if not ok or type(input) ~= "table" then
    return "(none)"
  end
  local keys = {}
  for k, v in pairs(input) do
    keys[#keys + 1] = tostring(k) .. "=" .. tostring(v)
  end
  table.sort(keys)
  return "{" .. table.concat(keys, ", ") .. "}"
end

local function find_port()
  for port = 0, 3 do
    local ok, input = pcall(emu.getInput, port, 0)
    emu.log(string.format("zapper_record.lua: emu.getInput(%d) = %s", port, describe(port)))
    if PORT == nil and ok and type(input) == "table" and input.x ~= nil and input.y ~= nil then
      PORT = port
    end
  end
  if PORT == nil then
    emu.log("zapper_record.lua: NO PORT HAS A ZAPPER (a table with x and y). Settings -> Input: port 2 (or the")
    emu.log("expansion port, for a Famicom game) must be a Zapper; the tables above show what each port has now.")
  else
    emu.log("zapper_record.lua: the Zapper is on getInput port " .. PORT)
  end
end

emu.addEventCallback(function()
  if PORT == nil then
    find_port()
    if PORT == nil then
      return
    end
  end
  local input = emu.getInput(PORT, 0)
  local fire = input.trigger
  if fire == nil then
    fire = input.fire -- (another device type may name the trigger differently)
  end
  local text = aim_text(input.x, input.y, fire)
  if text ~= last then
    last = text
    lines = lines + 1
    file:write(string.format("%d zapper %s\n", emu.getCpuCycleCount(), text))
    file:flush()
    emu.log(string.format("%d zapper %s   (raw x=%s y=%s)", emu.getCpuCycleCount(), text, tostring(input.x),
      tostring(input.y)))
  end
end, emu.eventType.inputPolled)
emu.addEventCallback(function() file:close() end, emu.eventType.scriptEnded)
emu.log("zapper_record.lua: recording the Zapper aim to " .. path)
