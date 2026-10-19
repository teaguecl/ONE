/*
  SPDX-License-Identifier: GPL-2.0-or-later
  ONE NES emulator
  Copyright (C) 1999-2026 Chris Teague

  ONE is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  ONE is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with ONE; if not, see <https://www.gnu.org/licenses/>.
*/
// The emulator core is C. This is the only place the debugger includes its headers.
#pragma once

extern "C" {
// IWYU pragma: begin_exports
#include "cpu.h"
#include "ppu.h"
#include "rp2a03.h"
#include "mapper.h"
#include "cheats.h"
#include "input.h"
#include "cfgfile.h"
#include "settings.h"
#include "audio.h"
#include "savestate.h"
#include "log.h"
#include "trace.h"
#include "video.h"
#include "scheduler.h"
#include "emu.h"
#include "script.h"
#include "debug_hooks.h"
#include "version.h"
#include "debugger_priv.h" // 6502 opcode tables (trace.c)
// IWYU pragma: end_exports
}
