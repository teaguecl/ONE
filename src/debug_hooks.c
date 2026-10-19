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
// Debugger hooks (compiled only into the debugger build of the core, see debug_hooks.h).
#include <stddef.h>
#include "debug_hooks.h"
#include "cpu.h"
#include "scheduler.h"

dbg_boundary_hook_t dbg_boundary_hook = NULL;
dbg_access_hook_t dbg_access_hook = NULL;
dbg_jam_hook_t dbg_jam_hook = NULL;
dbg_key_hook_t dbg_key_hook = NULL;
dbg_dmc_hook_t dbg_dmc_hook = NULL;

void dbg_set_dmc_hook(dbg_dmc_hook_t hook)
{
    dbg_dmc_hook = hook;
}

void dbg_set_key_hook(dbg_key_hook_t hook)
{
    dbg_key_hook = hook;
}

void dbg_set_hooks(dbg_boundary_hook_t boundary, dbg_access_hook_t access, dbg_jam_hook_t jam)
{
    dbg_boundary_hook = boundary;
    dbg_access_hook = access;
    dbg_jam_hook = jam;
}

bool dbg_resume(void)
{
    return sched_resume(cpu_get_thread());
}
