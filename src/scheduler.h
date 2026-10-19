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
#ifndef scheduler_h_defined
#define scheduler_h_defined

#include <stdbool.h>
#include "libco/libco.h"

void sched_enter(cothread_t start_thread); // run the emulation from the host thread until sched_exit()
void sched_exit(int reason);               // (emulation thread) end the emulation, back to the host thread

// Debugger support: pause the emulation and resume it later.
void sched_break(void);               // (emulation thread) pause, back to the host thread
bool sched_resume(cothread_t thread); // (host thread) continue thread until it pauses or exits;
                                      // returns false once the emulation has exited
void sched_reset(void);               // forget the host thread / exit state (before a new power-on)

#endif
