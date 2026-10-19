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
#ifndef debug_hooks_h_defined
#define debug_hooks_h_defined

// Hooks for the debugger (one-debugger). Only the debugger build of the core (ONE_DEBUGGER) calls them;
// the normal build has no hooks and no overhead.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called at every instruction boundary (before the opcode fetch) with the address of the next instruction.
// Return true to pause the emulation there; dbg_resume() continues it.
typedef bool (*dbg_boundary_hook_t)(uint16_t pc);
// Called after every CPU bus read or write (not for the debugger's own side-effect-free reads).
typedef void (*dbg_access_hook_t)(uint16_t address, uint8_t value, bool is_write);

// Called when the DMC channel reads a sample byte from the bus (DMA: not seen by dbg_access_hook_t)
typedef void (*dbg_dmc_hook_t)(uint16_t address);

// Called when the CPU executes an opcode the core does not implement (JAM/KIL); the emulation then pauses.
typedef void (*dbg_jam_hook_t)(uint16_t pc, uint8_t opcode);

extern dbg_boundary_hook_t dbg_boundary_hook;
extern dbg_access_hook_t dbg_access_hook;
extern dbg_jam_hook_t dbg_jam_hook;
extern dbg_dmc_hook_t dbg_dmc_hook;

void dbg_set_hooks(dbg_boundary_hook_t boundary, dbg_access_hook_t access, dbg_jam_hook_t jam);
void dbg_set_dmc_hook(dbg_dmc_hook_t hook);

// Debugger keys pressed in the game window (input.c), so the debugger can be driven from there too.
// key: 5, 8, 10, 12 = F5, F8, F10, F12; DBG_KEY_CTRL_D = Ctrl+D, DBG_KEY_ALT_X, DBG_KEY_QUIT = window closed.  repeat: the key is being held down (auto-repeat).
#define DBG_KEY_CTRL_D 100
#define DBG_KEY_QUIT   101 // the game window was closed (its X button)
#define DBG_KEY_ALT_X  102 // Alt+X in the game window: quit the debugger
typedef void (*dbg_key_hook_t)(int key, bool repeat);
extern dbg_key_hook_t dbg_key_hook;
void dbg_set_key_hook(dbg_key_hook_t hook);

// Run the emulation until the boundary hook pauses it. Returns false once the emulation has ended
// (window closed, -k reached); call emu_power_on() to start again.
bool dbg_resume(void);

#ifdef __cplusplus
}
#endif

#endif
