-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- apu_log.lua: logs Mesen's APU state once per frame, as reference data for ONE's audio (tools/mesen/README.md).
--
-- Writes every field of emu.getState() whose name contains "apu" (all five channels: timers, duty, envelope,
-- sweep, length and linear counters, output levels, ...), one line per frame, to a text file next to this
-- script.  It does not need to know Mesen's field names, so it works with Mesen 2 (flat "apu.square1.x" keys)
-- and Mesen 1 (nested tables) alike.
--
-- Settings (change them here):
local FRAMES = 1800            -- how many frames to log (1800 = 30 seconds)
local OUT_NAME = nil           -- output file; nil = "apu_<rom name>.txt" in the folder of this script

local function flatten(t, prefix, out)
  for k, v in pairs(t) do
    local name = (prefix == "") and tostring(k) or (prefix .. "." .. tostring(k))
    if type(v) == "table" then
      flatten(v, name, out)
    elseif type(v) ~= "function" then
      out[name] = v
    end
  end
end

local function value_text(v)
  if type(v) == "boolean" then return v and "1" or "0" end
  return tostring(v)
end

-- the same set of keys, in the same order, on every line
local keys = nil
local function apu_keys(state)
  local list = {}
  for k, _ in pairs(state) do
    if string.find(string.lower(k), "apu", 1, true) then table.insert(list, k) end
  end
  table.sort(list)
  return list
end

local function find_any(state, names)
  for _, n in ipairs(names) do
    if state[n] ~= nil then return state[n] end
  end
  return "?"
end

local rom = "rom"
local ok, info = pcall(emu.getRomInfo)
if ok and info and info.name then rom = info.name end
rom = string.gsub(string.gsub(rom, "%.[Nn][Ee][Ss]$", ""), "[^%w%-_%.]", "_")
local folder = ""
local src = debug and debug.getinfo and debug.getinfo(1, "S")
if src and src.source and string.sub(src.source, 1, 1) == "@" then
  folder = string.match(string.sub(src.source, 2), "^(.*[/\\])") or ""
end
if folder == "" and os ~= nil and os.getenv ~= nil then
  local home = os.getenv("HOME") or os.getenv("USERPROFILE")
  if home then folder = home .. "/" end   -- the script's folder is unknown: the home folder instead
end
local path = OUT_NAME or (folder .. "apu_" .. rom .. ".txt")

if io == nil or io.open == nil then
  emu.log("apu_log.lua: file access is off. In the Script Window, open Settings and tick")
  emu.log("'Allow access to I/O and OS functions', then run the script again.")
  return
end
local file, err = io.open(path, "w")
if file == nil then
  emu.log("apu_log.lua: cannot write " .. path .. ": " .. tostring(err))
  return
end
file:write("# Mesen APU state, one line per frame (tools/mesen/apu_log.lua); ROM: " .. rom .. "\n")

local frames = 0
local callback
callback = emu.addEventCallback(function()
  local state = {}
  flatten(emu.getState(), "", state)
  if keys == nil then
    keys = apu_keys(state)
    file:write("# fields: frame cycle " .. table.concat(keys, " ") .. "\n")
    emu.log("apu_log.lua: " .. #keys .. " APU fields, writing " .. FRAMES .. " frames to " .. path)
  end
  local parts = {
    "frame=" .. value_text(find_any(state, {"ppu.frameCount", "frameCount"})),
    "cycle=" .. value_text(find_any(state, {"cpu.cycleCount", "cycleCount"})),
  }
  for _, k in ipairs(keys) do
    table.insert(parts, k .. "=" .. value_text(state[k]))
  end
  file:write(table.concat(parts, " ") .. "\n")
  frames = frames + 1
  if frames % 60 == 0 then file:flush() end
  if frames >= FRAMES then
    file:close()
    emu.removeEventCallback(callback, emu.eventType.endFrame)
    emu.log("apu_log.lua: done, " .. frames .. " frames in " .. path)
  end
end, emu.eventType.endFrame)
