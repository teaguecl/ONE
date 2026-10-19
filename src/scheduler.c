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
#include <stdlib.h>
#include <assert.h>
#include "libco/libco.h"
#include "scheduler.h"

static cothread_t host_thread = NULL;
static int exit_reason;
static bool exited = false;

void sched_enter(cothread_t start_thread)
{
    assert(host_thread == NULL);
    host_thread = co_active(); /* save off the main program thread */
    co_switch(start_thread);   /* start the CPU running */
}

void sched_exit(int reason)
{
    exit_reason = reason;
    exited = true;
    co_switch(host_thread);
}

void sched_break(void)
{
    co_switch(host_thread);
}

bool sched_resume(cothread_t thread)
{
    if (exited) {
        return false;
    }
    if (host_thread == NULL) {
        host_thread = co_active();
    }
    co_switch(thread);
    return !exited;
}

void sched_reset(void)
{
    host_thread = NULL;
    exited = false;
}
