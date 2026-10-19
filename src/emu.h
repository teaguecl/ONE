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
#ifndef emu_h_defined
#define emu_h_defined

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Power on the NES with the given ROM: create the CPU/PPU/APU threads, power-on state, load the cartridge, reset.
// Can be called again to power-cycle (the previous cartridge is unloaded first). Does not start emulating:
// the host then runs sched_enter(cpu_get_thread()) or, in the debugger, dbg_resume().
bool emu_power_on(char *rom_filename);
// The same with the ROM file in memory (the libretro core); NAME is for messages and the slot / cheat file names
bool emu_power_on_memory(const uint8_t *data, size_t size, char *name);
// Unload the cartridge (writes the .sav file) and delete the threads.
void emu_power_off(void);

#endif
