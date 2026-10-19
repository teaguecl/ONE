-- SPDX-License-Identifier: GPL-2.0-or-later
-- Copyright (C) 1999-2026 Chris Teague
-- Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
-- FCEUX's Lua API on top of ONE's Mesen 2 style API (script.c). Built into ONE and run in every script's Lua state
-- before the script itself, so scripts written for FCEUX (memory.readbyte, gui.text, joypad.set, a main loop with
-- emu.frameadvance()) run unchanged, and Mesen style scripts are not affected. FCEUX's Lua is 5.1: a few 5.1 names
-- are provided too. The reference is FCEUX's Lua function list (fceux.com/web/help/LuaFunctionsList.html).
local emu, one = emu, one
local M, E, C = emu.memType, emu.eventType, emu.callbackType

-- --- Lua 5.1 names ---------------------------------------------------------------------------------------------
unpack = unpack or table.unpack
loadstring = loadstring or load
table.getn = table.getn or function(t) return #t end
math.pow = math.pow or function(a, b) return a ^ b end
math.mod = math.mod or math.fmod
string.gfind = string.gfind or string.gmatch
table.maxn = table.maxn or function(t)
  local n = 0
  for k in pairs(t) do if type(k) == "number" and k > n then n = k end end
  return n
end
table.foreach = table.foreach or function(t, f)
  for k, v in pairs(t) do local r = f(k, v); if r ~= nil then return r end end
end
table.foreachi = table.foreachi or function(t, f)
  for i = 1, #t do local r = f(i, t[i]); if r ~= nil then return r end end
end
table.setn = table.setn or function() end
do
  -- Lua 5.1's table.remove(t, pos) with pos past the end did nothing; 5.4 raises an error
  local remove = table.remove
  table.remove = function(t, ...)
    if select("#", ...) > 0 then
      local pos = ...
      if type(pos) == "number" and (pos < 1 or pos > #t + 1) and not (pos == 0 and #t == 0) then return nil end
    end
    return remove(t, ...)
  end
end
math.log10 = math.log10 or function(x) return math.log(x, 10) end
math.atan2 = math.atan2 or math.atan
math.ldexp = math.ldexp or function(m, e) return m * 2.0 ^ e end
math.frexp = math.frexp or function(x)
  if x == 0 then return 0.0, 0 end
  local e = math.floor(math.log(math.abs(x), 2)) + 1
  return x / 2.0 ^ e, e
end
math.cosh = math.cosh or function(x) return (math.exp(x) + math.exp(-x)) / 2 end
math.sinh = math.sinh or function(x) return (math.exp(x) - math.exp(-x)) / 2 end
math.tanh = math.tanh or function(x) return math.sinh(x) / math.cosh(x) end
do
  -- Lua 5.1's string.format("%d", 2.5) printed 2; 5.4 raises an error: integer formats get truncated numbers
  local format = string.format
  string.format = function(fmt, ...)
    local args, n = table.pack(...), 0
    for spec in tostring(fmt):gmatch("%%[-+ #0]*%d*%.?%d*([%a%%])") do
      if spec ~= "%" then
        n = n + 1
        local v = args[n]
        if type(v) == "number" and spec:match("[dicoxXu]") and math.type(v) == "float" then
          args[n] = (v >= 0) and math.floor(v) or math.ceil(v)
          if spec == "u" then args[n] = math.abs(args[n]) end
        end
      end
    end
    return format((fmt:gsub("%%(%d*)u", "%%%1d")), table.unpack(args, 1, args.n))
  end
end

-- --- bit operations (FCEUX globals, and LuaBitOp's bit table) -----------------------------------------------------
local function fold(op, ...)
  local r = select(1, ...) or 0
  for i = 2, select("#", ...) do r = op(r, (select(i, ...))) end
  return math.tointeger(r) or math.floor(r)
end
function AND(...) return fold(function(a, b) return math.floor(a) & math.floor(b) end, ...) end
function OR(...) return fold(function(a, b) return math.floor(a) | math.floor(b) end, ...) end
function XOR(...) return fold(function(a, b) return math.floor(a) ~ math.floor(b) end, ...) end
function BIT(...)
  local r = 0
  for i = 1, select("#", ...) do r = r | (1 << math.floor((select(i, ...)))) end
  return r
end
-- SHIFT(x, n): right by n, left by -n (as LuaBitOp's rshift / lshift: 32 bits)
function SHIFT(x, n)
  x = math.floor(x) & 0xFFFFFFFF
  n = math.floor(n)
  local r = (n >= 0) and (x >> (n & 31)) or ((x << ((-n) & 31)) & 0xFFFFFFFF)
  return (r >= 0x80000000) and (r - 0x100000000) or r
end
function tobitstring(x)
  local t = {}
  x = math.floor(x)
  for i = 7, 0, -1 do t[#t + 1] = ((x >> i) & 1 == 1) and "1" or "0" end
  return table.concat(t)
end
function copytable(t)
  if type(t) ~= "table" then return t end
  local c = {}
  for k, v in pairs(t) do c[k] = v end
  return setmetatable(c, getmetatable(t))
end
function addressof(v) return tonumber(tostring(v):match("0x(%x+)") or "0", 16) end
local function tobit(x)
  x = math.floor(x) & 0xFFFFFFFF
  return (x >= 0x80000000) and (x - 0x100000000) or x
end
bit = bit or {
  tobit = tobit,
  band = function(...) return tobit(fold(function(a, b) return math.floor(a) & math.floor(b) end, ...)) end,
  bor = function(...) return tobit(fold(function(a, b) return math.floor(a) | math.floor(b) end, ...)) end,
  bxor = function(...) return tobit(fold(function(a, b) return math.floor(a) ~ math.floor(b) end, ...)) end,
  bnot = function(a) return tobit(~math.floor(a)) end,
  lshift = function(a, n) return tobit((math.floor(a) & 0xFFFFFFFF) << (n & 31)) end,
  rshift = function(a, n) return tobit((math.floor(a) & 0xFFFFFFFF) >> (n & 31)) end,
  arshift = function(a, n) return tobit(tobit(a) >> (n & 31) | ((tobit(a) < 0 and n > 0) and ~(0xFFFFFFFF >> (n & 31)) or 0)) end,
  rol = function(a, n) a = math.floor(a) & 0xFFFFFFFF; n = n & 31; return tobit((a << n) | (a >> (32 - n))) end,
  ror = function(a, n) a = math.floor(a) & 0xFFFFFFFF; n = n & 31; return tobit((a >> n) | (a << (32 - n))) end,
  tohex = function(a, n) n = n or 8; return string.format("%0" .. math.abs(n) .. (n < 0 and "X" or "x"), math.floor(a) & ((1 << (4 * math.abs(n))) - 1)) end,
}

-- --- memory, rom, ppu -------------------------------------------------------------------------------------------
memory = {}
function memory.readbyte(a) return emu.read(a, M.nesMemory) end
memory.readbyteunsigned = memory.readbyte
function memory.readbytesigned(a) return emu.read(a, M.nesMemory, true) end
function memory.readword(lo, hi)
  return emu.read(lo, M.nesMemory) | (emu.read(hi or (lo + 1), M.nesMemory) << 8)
end
memory.readwordunsigned = memory.readword
function memory.readwordsigned(lo, hi)
  local v = memory.readword(lo, hi)
  return (v >= 0x8000) and (v - 0x10000) or v
end
function memory.readbyterange(a, length)
  local t = {}
  for i = 0, length - 1 do t[#t + 1] = string.char(emu.read(a + i, M.nesMemory)) end
  return table.concat(t)
end
function memory.writebyte(a, v) emu.write(a, math.floor(v) % 256, M.nesMemory) end
local registers = { a = "cpu.a", x = "cpu.x", y = "cpu.y", s = "cpu.sp", p = "cpu.ps", pc = "cpu.pc" }
function memory.getregister(name) return emu.getState()[registers[string.lower(name)] or ""] end
function memory.setregister(name, value)
  local key = registers[string.lower(name)]
  if key then emu.setState({ [key] = math.floor(value) }) end
end

-- memory.register*(address, [size,] function): FCEUX calls func(address, size, value); func nil removes it
local hooks = {}
local function register(kind, address, size, func)
  if type(size) == "function" or size == nil then size, func = 1, size end
  local key = kind .. ":" .. address
  local old = hooks[key]
  if old then emu.removeMemoryCallback(old.ref, old.type, old.first, old.last) end
  hooks[key] = nil
  if func then
    local first, last = address, math.min(address + size - 1, 0xFFFF)
    local ref = emu.addMemoryCallback(function(a, v)
      func(a, 1, kind == "exec" and 0 or v)
      -- (nothing returned: FCEUX callbacks do not change values)
    end, C[kind], first, last)
    hooks[key] = { ref = ref, type = C[kind], first = first, last = last }
  end
end
function memory.registerwrite(address, size, func) register("write", address, size, func) end
function memory.registerread(address, size, func) register("read", address, size, func) end
function memory.registerexec(address, size, func) register("exec", address, size, func) end
memory.register = memory.registerwrite
memory.registerrun = memory.registerexec
memory.registerexecute = memory.registerexec

rom = {}
local rom_file
local function rom_data()
  if not rom_file then
    local f = io.open(emu.getRomInfo().path, "rb")
    rom_file = f and f:read("a") or ""
    if f then f:close() end
  end
  return rom_file
end
function rom.readbyte(a) return rom_data():byte(a + 1) or 0 end -- (FCEUX: the file, iNES header included)
rom.readbyteunsigned = rom.readbyte
function rom.readbytesigned(a)
  local v = rom.readbyte(a)
  return (v >= 0x80) and (v - 0x100) or v
end
function rom.writebyte() print("rom.writebyte: not supported by ONE") end
function rom.getfilename() return (emu.getRomInfo().name:gsub("%.[^.]*$", "")) end
function rom.gethash() return emu.getRomInfo().fileCrc32Hash end -- (CRC-32: ONE has no MD5)

ppu = {}
function ppu.readbyte(a) return emu.read(a, M.nesPpuMemory) end
function ppu.readbyterange(a, length)
  local t = {}
  for i = 0, length - 1 do t[#t + 1] = string.char(emu.read(a + i, M.nesPpuMemory)) end
  return table.concat(t)
end

-- --- joypad, zapper, input --------------------------------------------------------------------------------------
joypad = {}
local fceux_buttons = { A = "a", B = "b", select = "select", start = "start", up = "up", down = "down",
                        left = "left", right = "right" }
function joypad.get(player)
  local input, t = emu.getInput((player or 1) - 1), {}
  for k, v in pairs(fceux_buttons) do t[k] = input[v] end
  return t
end
joypad.read = joypad.get
joypad.getimmediate = joypad.get
joypad.readimmediate = joypad.get
function joypad.getdown(player)
  local t = {}
  for k, v in pairs(joypad.get(player)) do if v then t[k] = true end end
  return t
end
joypad.readdown = joypad.getdown
function joypad.getup(player)
  local t = {}
  for k, v in pairs(joypad.get(player)) do if not v then t[k] = false end end
  return t
end
joypad.readup = joypad.getup
-- joypad.set(player, buttons): true / false force a button, "invert" flips the player's, nil leaves it; for the
-- next frame (applied when the frame's input is read)
local joypad_sets, apply_sets = {}, nil
local apply_joypad_sets -- (below: registered for inputPolled by the first joypad.set)
function joypad.set(player, buttons)
  joypad_sets[player or 1] = buttons
  if not apply_sets then apply_sets = emu.addEventCallback(apply_joypad_sets, E.inputPolled) end
end
joypad.write = joypad.set
apply_joypad_sets = function()
  for player, buttons in pairs(joypad_sets) do
    local input, out = emu.getInput(player - 1), {}
    for k, v in pairs(fceux_buttons) do
      local want = buttons[k]
      if want == nil then want = buttons[v] end
      if want == "invert" then out[v] = not input[v]
      elseif want ~= nil then out[v] = (want == true or want == 1) end
    end
    emu.setInput(out, player - 1)
  end
  joypad_sets = {}
end

zapper = {}
function zapper.read()
  local m = emu.getMouseState()
  return { x = m.x, y = m.y, fire = m.left and 1 or 0 }
end
-- zapper.set{x=, y=, fire=} (also xmouse / ymouse / click): where the Zapper points this frame; x, y outside the picture:
-- away from the screen. Call it from the main loop each frame (like joypad.set).
function zapper.set(t)
  t = t or {}
  local x, y = t.x or t.xmouse, t.y or t.ymouse
  local fire = t.fire
  if fire == nil then fire = t.click end
  local trigger = nil
  if fire ~= nil then trigger = (fire == true or fire == 1) end
  emu.setInput({ x = x, y = y, trigger = trigger }, 1)
end

input = {}
-- FCEUX key names -> SDL key names
local keys = {
  shift = { "Left Shift", "Right Shift" }, control = { "Left Ctrl", "Right Ctrl" }, alt = { "Left Alt", "Right Alt" },
  capslock = "CapsLock", numlock = "Numlock", scrolllock = "ScrollLock", backspace = "Backspace", tab = "Tab",
  enter = "Return", escape = "Escape", space = "Space", pageup = "PageUp", pagedown = "PageDown", ["end"] = "End",
  home = "Home", left = "Left", up = "Up", right = "Right", down = "Down", insert = "Insert", delete = "Delete",
  numpad0 = "Keypad 0", numpad1 = "Keypad 1", numpad2 = "Keypad 2", numpad3 = "Keypad 3", numpad4 = "Keypad 4",
  numpad5 = "Keypad 5", numpad6 = "Keypad 6", numpad7 = "Keypad 7", numpad8 = "Keypad 8", numpad9 = "Keypad 9",
  ["numpad*"] = "Keypad *", ["numpad+"] = "Keypad +", ["numpad-"] = "Keypad -", ["numpad."] = "Keypad .",
  ["numpad/"] = "Keypad /", semicolon = ";", plus = "=", minus = "-", comma = ",", period = ".", slash = "/",
  tilde = "`", leftbracket = "[", backslash = "\\", rightbracket = "]", quote = "'",
}
for c = string.byte("A"), string.byte("Z") do keys[string.char(c)] = string.char(c) end
for d = 0, 9 do keys[tostring(d)] = tostring(d) end
for f = 1, 24 do keys["F" .. f] = "F" .. f end
function input.get()
  local m = emu.getMouseState()
  local t = { xmouse = m.x, ymouse = m.y, leftclick = m.left or nil, rightclick = m.right or nil,
              middleclick = m.middle or nil }
  for name, sdl in pairs(keys) do
    if type(sdl) == "table" then
      if emu.isKeyPressed(sdl[1]) or emu.isKeyPressed(sdl[2]) then t[name] = true end
    elseif emu.isKeyPressed(sdl) then
      t[name] = true
    end
  end
  return t
end
input.read = input.get
function input.popup(message, kind) print(message); return (kind == "yesno" or kind == "yesnocancel") and "yes" or "ok" end

-- --- save states ------------------------------------------------------------------------------------------------
-- (taken at the end of the picture and loaded at the next instruction: savestate.load needs a state saved earlier)
savestate = {}
function savestate.object() return one.newState() end
savestate.create = savestate.object
function savestate.save(state) one.saveState(state) end
function savestate.load(state) emu.loadSavestate(state) end
function savestate.persist() end
local save_ref, load_ref, save_func, load_func
function savestate.registersave(func)
  local old = save_func
  if save_ref then emu.removeEventCallback(save_ref, E.stateSaved) end
  save_func, save_ref = func, func and emu.addEventCallback(func, E.stateSaved)
  return old
end
function savestate.registerload(func)
  local old = load_func
  if load_ref then emu.removeEventCallback(load_ref, E.stateLoaded) end
  load_func, load_ref = func, func and emu.addEventCallback(func, E.stateLoaded)
  return old
end
function savestate.loadscriptdata() return nil end

movie = {}
function movie.active() return false end
function movie.mode() return nil end
function movie.framecount() return emu.framecount() end
function movie.rerecordcount() return 0 end
function movie.readonly() return false end
movie.getreadonly = movie.readonly
function movie.setreadonly() end
function movie.recording() return false end
function movie.playing() return false end
function movie.ispoweron() return true end
function movie.isfromsavestate() return false end
function movie.rerecordcounting() end
for _, name in ipairs({ "play", "playback", "load", "record", "save", "stop", "close", "length", "name", "getname",
                        "getfilename", "replay", "playbeginning" }) do
  movie[name] = function() print("movie." .. name .. ": ONE has no movies") return nil end
end

-- --- gui ----------------------------------------------------------------------------------------------------------
gui = {}
local opacity = 255 -- (FCEUX's transparencyModifier: 255 = as given)
local named = {
  white = 0xFFFFFFFF, black = 0x000000FF, clear = 0x00000000, gray = 0x7F7F7FFF, grey = 0x7F7F7FFF,
  red = 0xFF0000FF, orange = 0xFF7F00FF, yellow = 0xFFFF00FF, chartreuse = 0x7FFF00FF, green = 0x00FF00FF,
  teal = 0x00FF7FFF, cyan = 0x00FFFFFF, blue = 0x0000FFFF, purple = 0x7F00FFFF, magenta = 0xFF00FFFF,
}
-- FCEUX color -> 0xRRGGBBAA, or nil: a number (0xRRGGBBAA), "#rrggbb" / "#rrggbbaa" (shorter: padded with zeros
-- on the right), a name, "rand...", "P00"-"P3F" (NES palette), {r, g, b, a} or {r=, g=, b=, a=} (a: 255)
local function rgba(c)
  local t = type(c)
  if t == "number" then
    return math.floor(c) & 0xFFFFFFFF
  elseif t == "string" then
    if c:sub(1, 1) == "#" then
      local hex = c:sub(2):match("^%x*")
      local v = tonumber(hex:sub(1, 8), 16) or 0
      local missing = math.max(0, 8 - #hex)
      v = (v << (4 * missing)) & 0xFFFFFFFF
      if missing >= 2 then v = v | 0xFF end
      return v
    elseif c:sub(1, 1) == "P" and c:match("^P%x%x?$") then
      local rgb = one.paletteColor(tonumber(c:sub(2), 16) & 0x3F)
      return ((rgb & 0xFFFFFF) << 8) | 0xFF
    elseif c:lower():sub(1, 4) == "rand" then
      return (math.random(0, 0xFFFFFF) << 8) | 0xFF
    end
    return named[c:lower()]
  elseif t == "table" then
    local v = 0xFF
    for k, x in pairs(c) do
      x = math.max(0, math.min(255, math.floor(tonumber(x) or 0)))
      local key = type(k) == "string" and k:sub(1, 1):lower() or k
      if key == 1 or key == "r" then v = v | (x << 24)
      elseif key == 2 or key == "g" then v = v | (x << 16)
      elseif key == 3 or key == "b" then v = v | (x << 8)
      elseif key == 4 or key == "a" then v = (v & ~0xFF) | x end
    end
    return v
  end
  return nil
end
function gui.parsecolor(c)
  local v = rgba(c) or error("unknown color " .. tostring(c), 2)
  return (v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF
end
-- color argument (DEFAULT: 0xRRGGBBAA when it is optional) -> Mesen color (alpha inverted), gui.opacity applied
local function mesen(c, default)
  local v = rgba(c)
  if v == nil then
    if default == nil then error("invalid color " .. tostring(c), 3) end
    v = default
  end
  local a = math.min(255, ((v & 0xFF) * opacity) // 255)
  return ((255 - a) << 24) | (v >> 8)
end
function gui.opacity(alpha) opacity = math.max(0, math.floor(alpha * 255)) end
gui.setopacity = gui.opacity
function gui.transparency(trans) opacity = math.max(0, math.floor((4 - trans) / 4 * 255)) end
function gui.pixel(x, y, color) emu.drawPixel(math.floor(x), math.floor(y), mesen(color)) end
gui.drawpixel, gui.setpixel, gui.writepixel = gui.pixel, gui.pixel, gui.pixel
function gui.line(x1, y1, x2, y2, color, skipfirst)
  local c = mesen(color, 0xFFFFFFFF)
  x1, y1, x2, y2 = math.floor(x1), math.floor(y1), math.floor(x2), math.floor(y2)
  if skipfirst and (x1 ~= x2 or y1 ~= y2) then
    -- (FCEUX leaves out the first point: start one step along the line)
    local dx, dy = x2 - x1, y2 - y1
    local n = math.max(math.abs(dx), math.abs(dy))
    x1, y1 = x1 + math.floor(dx / n + 0.5), y1 + math.floor(dy / n + 0.5)
  end
  emu.drawLine(x1, y1, x2, y2, c)
end
gui.drawline = gui.line
-- gui.box(x1, y1, x2, y2, fill, outline): fill defaults to white at 63/255, outline to the fill color, opaque
function gui.box(x1, y1, x2, y2, fill, outline)
  x1, y1, x2, y2 = math.floor(x1), math.floor(y1), math.floor(x2), math.floor(y2)
  if x1 > x2 then x1, x2 = x2, x1 end
  if y1 > y2 then y1, y2 = y2, y1 end
  local fill_rgba = rgba(fill) or 0xFFFFFF3F
  local w, h = x2 - x1 + 1, y2 - y1 + 1
  emu.drawRectangle(x1, y1, w, h, mesen(outline, (fill_rgba & 0xFFFFFF00) | 0xFF), false)
  if w > 2 and h > 2 then emu.drawRectangle(x1 + 1, y1 + 1, w - 2, h - 2, mesen(fill_rgba), true) end
end
gui.drawbox, gui.rect, gui.drawrect = gui.box, gui.box, gui.box
-- gui.text(x, y, text, color, background): returns the x after the text. (ONE's font: 6 pixels a character, the
-- background 1 pixel around the letters, as FCEUX's; FCEUX's font is proportional)
function gui.text(x, y, str, textcolor, backcolor)
  x, y, str = math.floor(x), math.floor(y), tostring(str)
  local back = mesen(backcolor, 0x1B1269FF)
  emu.drawString(x, y - 1, str, mesen(textcolor, 0xFFFFFFFF), back)
  local longest, lines = 0, 0
  for line in (str .. "\n"):gmatch("(.-)\n") do longest, lines = math.max(longest, #line), lines + 1 end
  if longest > 0 then emu.drawRectangle(x - 1, y - 1, 1, 9 * lines, back, true) end -- (FCEUX's halo on the left)
  return x + 6 * longest
end
gui.drawtext = gui.text
function gui.getpixel(x, y)
  local c = one.hudPixel(x, y)
  return (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, (c >> 24) & 0xFF
end
local gui_ref, gui_func
function gui.register(func)
  local old = gui_func
  if gui_ref then emu.removeEventCallback(gui_ref, E.endFrame) end
  gui_func, gui_ref = func, func and emu.addEventCallback(func, E.endFrame)
  return old
end
function gui.savescreenshot() return one.screenshot() end
function gui.savescreenshotas(name) return one.screenshot(name) end
-- gd image format (FCEUX's screenshots and overlays): 11-byte header, then A R G B per pixel (A: 0 opaque..127)
function gui.gdscreenshot()
  local pixels, out = emu.getScreenBuffer(), { "\255\254\1\0\0\240\1\255\255\255\255" }
  for i = 1, #pixels do
    local c = pixels[i]
    out[#out + 1] = string.char(0, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF)
  end
  return table.concat(out)
end
function gui.gdoverlay(...)
  local args = { ... }
  local dx, dy, i = 0, 0, 1
  if type(args[1]) == "number" then dx, dy, i = args[1], args[2], 3 end
  local img = args[i]
  local alphamul = args[#args] ~= img and type(args[#args]) == "number" and #args > i + 4 and args[#args] or 1
  if type(img) ~= "string" or #img < 11 then return end
  local w = (img:byte(3) << 8) | img:byte(4)
  local h = (img:byte(5) << 8) | img:byte(6)
  local pos = 12
  for y = 0, h - 1 do
    for x = 0, w - 1 do
      local a, r, g, b = img:byte(pos, pos + 3)
      pos = pos + 4
      if a and a < 127 then
        local alpha = math.floor((127 - a) * 255 / 127 * alphamul * opacity / 255)
        if alpha > 0 then emu.drawPixel(dx + x, dy + y, ((255 - alpha) << 24) | (r << 16) | (g << 8) | b) end
      end
    end
  end
end
gui.image, gui.drawimage = gui.gdoverlay, gui.gdoverlay
gui.popup = input.popup

-- --- sound, debugger, taseditor -------------------------------------------------------------------------------
sound = {}
-- sound.get(): FCEUX's table, from one.apuState(); volumes 0-1, frequencies in Hz, midikey (69 = A 440 Hz)
local NTSC_CPU = 1789772.7272
local NOISE_PERIODS = { 4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 }
local DMC_PERIODS = { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 }
local function midikey(freq) return math.log(freq / 440.0) * 12 / math.log(2.0) + 69 end
function sound.get()
  local st = one.apuState()
  local r = {}
  for i, name in ipairs({ "square1", "square2" }) do
    local c = st["pulse" .. i]
    local freq = NTSC_CPU / 16.0 / (c.period + 1)
    r[name] = { volume = (c.muted or c.length == 0) and 0.0 or c.volume / 15.0, frequency = freq,
                midikey = midikey(freq), duty = c.duty, regs = { frequency = c.period } }
  end
  local t = st.triangle
  local freq = NTSC_CPU / 32.0 / (t.period + 1)
  r.triangle = { volume = (t.length == 0 or t.linear == 0) and 0.0 or 1.0, frequency = freq, midikey = midikey(freq),
                 regs = { frequency = t.period } }
  local n = st.noise
  freq = NTSC_CPU / NOISE_PERIODS[n.period + 1]
  if n.short then freq = freq / 93.0 end
  r.noise = { volume = n.length == 0 and 0.0 or n.volume / 15.0, short = n.short, frequency = freq,
              midikey = midikey(freq), regs = { frequency = n.period } }
  local d = st.dmc
  freq = NTSC_CPU / DMC_PERIODS[d.rate + 1]
  r.dpcm = { volume = d.remaining > 0 and 1.0 or 0.0, frequency = freq, midikey = midikey(freq),
             dmcaddress = d.address, dmcsize = d.length, dmcloop = d.loop, dmcseed = d.output,
             regs = { frequency = d.rate } }
  return { rp2a03 = r }
end
debugger = debugger or {}
function debugger.hitbreakpoint() if emu.breakExecution then emu.breakExecution() end end
function debugger.getcyclescount() return emu.getState()["cpu.cycleCount"] end
function debugger.getinstructionscount() return 0 end
function debugger.resetcyclescount() end
function debugger.resetinstructionscount() end
function debugger.getsymboloffset() return -1 end
taseditor = { engaged = function() return false end }
setmetatable(taseditor, { __index = function() return function() end end })

-- --- auxlib / iuplua -----------------------------------------------------------------------------------------------
-- FCEUX for Windows ships IUP (a GUI toolkit) and auxlib.lua, which loads it and closes a script's dialogs when it
-- ends. ONE has no IUP: require("auxlib") gives a stand-in whose dialogs are never shown, so a script that draws on
-- the picture and only has a window for extras still runs (a script that is all window does nothing useful).
do
  local warned = false
  -- an attribute that was never set, or a method: calling it does nothing, as a string it is ""
  local unset = setmetatable({}, {
    __call = function() return nil end,
    __concat = function(a, b) return tostring(a) .. tostring(b) end,
    __tostring = function() return "" end,
    __index = function(self) return self end,
  })
  local element = { __index = function() return unset end }
  local function make(kind)
    return function(attributes)
      if not warned then
        warned = true
        print("iup." .. kind .. ": ONE has no IUP; the script's windows are not shown")
      end
      return setmetatable(type(attributes) == "table" and attributes or {}, element)
    end
  end
  local stub = setmetatable({ CENTER = 0xFFFF, LEFT = 0xFFFE, RIGHT = 0xFFFD, MOUSEPOS = 0xFFFC,
                              CURRENT = 0xFFFB, CENTERPARENT = 0xFFFA, TOP = 0xFFFE, BOTTOM = 0xFFFD,
                              ANYWHERE = 0xFFFB, DEFAULT = -1, CLOSE = -3, IGNORE = -1, CONTINUE = -4,
                              Message = function(title, text) print(tostring(title) .. ": " .. tostring(text)) end,
                              Alarm = function(title, text) print(tostring(title) .. ": " .. tostring(text)) return 1 end,
                              GetFile = function() return nil, -1 end,
                              LoopStep = function() return 0 end, MainLoop = function() end, Close = function() end,
                              SetAttribute = function() end, GetAttribute = function() return nil end,
                              SetHandle = function() end, GetHandle = function() return nil end,
                              Destroy = function() end },
                            { __index = function(t, kind) local f = make(kind); rawset(t, kind, f); return f end })
  local function auxlib()
    iup = iup or stub
    handles = handles or {}
    dialogs = dialogs or 0
    return true
  end
  package.preload["auxlib"] = auxlib
  package.preload["iuplua"] = function() iup = iup or stub; return iup end
  package.preload["iupluacontrols"] = function() iup = iup or stub; return iup end
end

-- --- emu (FCEUX names; Mesen's are kept) ---------------------------------------------------------------------------
function emu.frameadvance()
  if not coroutine.isyieldable() then
    error("emu.frameadvance() works in the script's main loop, not in a callback", 2)
  end
  coroutine.yield()
end
function emu.framecount() return emu.getState().frameCount end
local lag_override
function emu.lagcount() return one.lagCount() end
function emu.lagged()
  if lag_override ~= nil then return lag_override end
  return one.isLagFrame()
end
local lag_ref
function emu.setlagflag(v)
  lag_override = v
  lag_ref = lag_ref or emu.addEventCallback(function() lag_override = nil end, E.startFrame)
end
function emu.emulating() return true end
function emu.paused() return false end
function emu.pause() if emu.breakExecution then emu.breakExecution() end end
function emu.unpause() end
function emu.speedmode() end
function emu.poweron() emu.reset() end -- (a RESET: ONE cannot power cycle from a script)
function emu.softreset() emu.reset() end
function emu.message(text) one.message(tostring(text)) end
function emu.print(...) print(...) end
function emu.getdir() return (emu.getRomInfo().path:match("^(.*)[/\\]") or ".") end
function emu.loadrom() print("emu.loadrom: not supported by ONE") end
function emu.addgamegenie() print("emu.addgamegenie: not supported by ONE yet") return false end
function emu.delgamegenie() return true end
function emu.getscreenpixel(x, y)
  local c = emu.getPixel(x, y)
  return (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, one.screenPaletteIndex(x, y)
end
function emu.exit() emu.stop(0) end
function emu.readonly() return false end
emu.getreadonly = emu.readonly
function emu.setreadonly() end
function emu.exec_count(_, func) return func() end
function emu.exec_time(_, func) return func() end
function emu.setrenderplanes() end
local function one_callback(event)
  local ref, func
  return function(f)
    local old = func
    if ref then emu.removeEventCallback(ref, event) end
    func, ref = f, f and emu.addEventCallback(f, event)
    return old
  end
end
emu.registerbefore = one_callback(E.startFrame)
emu.registerafter = one_callback(E.endFrame)
emu.registerexit = one_callback(E.scriptEnded)
FCEU = emu -- (very old scripts)

-- --- running the script ---------------------------------------------------------------------------------------
-- The main chunk runs as a coroutine: emu.frameadvance() yields, and it continues at the end of the next frame.
return function(main)
  local co = coroutine.create(main)
  local function step()
    local ok, err = coroutine.resume(co)
    if not ok then error(debug.traceback(co, tostring(err)), 0) end
    return coroutine.status(co) == "suspended"
  end
  if step() then
    local ref
    ref = emu.addEventCallback(function()
      if not step() then emu.removeEventCallback(ref, E.endFrame) end
    end, E.endFrame)
  end
end
