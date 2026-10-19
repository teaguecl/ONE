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
#ifndef trace_h_defined
#define trace_h_defined

#include <stdio.h>

// Mesen-format CPU trace (trace.c): one line per instruction, enabled with cpu_set_trace() / -t. It has a stream of
// its own (--trace FILE; stderr by default), which no other output shares: while it is stderr, log messages go to
// stdout (log.h).
void debugger_generate_trace(void);
void trace_set_file(FILE *file); // NULL: stderr

#endif
