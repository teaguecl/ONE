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
// Force-included when Lua is compiled for 64-bit Windows (CMakeLists.txt, target one_lua).
//
// Lua reports an error with longjmp. The CPU and PPU of ONE run on libco stacks (memory from malloc), and scripts run
// there, so an error() caught by pcall() jumps within such a stack. Windows' longjmp "unwinds" to the frame its setjmp
// recorded, with RtlUnwindEx, which refuses a frame outside the thread's own stack: STATUS_BAD_STACK (0xC0000028), or
// "RtlUnwindEx invalid frame" under Wine. So Lua's try / throw are made from RtlCaptureContext and RtlRestoreContext
// (without an exception record the latter is a plain jump back to the captured context, as the same compiler-agnostic
// setjmp / longjmp would be if they did not unwind). Lua has no C++ destructors to run on the way.
#ifndef ONE_LUA_SETJMP_NOUNWIND_H
#define ONE_LUA_SETJMP_NOUNWIND_H
#if defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdlib.h>
// (a thrown status is never 0, and lua_longjmp::status is volatile: it tells the second return from the capture)
#define LUAI_THROW(L, c) \
    do { \
        RtlRestoreContext(&(c)->b, NULL); \
        abort(); \
    } while (0)
#define LUAI_TRY(L, c, a) \
    RtlCaptureContext(&(c)->b); \
    if ((c)->status == 0) { \
        a \
    }
#define luai_jmpbuf CONTEXT
#endif
#endif
