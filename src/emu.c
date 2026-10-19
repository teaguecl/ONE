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
#include <stddef.h>
#include <stdint.h>
#include "log.h"
#include "emu.h"
#include "input.h"
#include "cpu.h"
#include "ppu.h"
#include "rp2a03.h"
#include "mapper.h"
#include "cheats.h"
#include "scheduler.h"
#include "video.h"
#include "savestate.h"
#include "frontend.h"
#include "menu_hint.h"
#include "script.h"

static bool cartridge_loaded = false;

// The time stamp of debug and trace messages (log.h)
static void emulation_time(uint32_t *frame, uint64_t *cpu_cycle, int *scanline, int *dot)
{
    *frame = ppu_debug_get_frame_num();
    *cpu_cycle = cpu_get_cycle();
    *scanline = (int)ppu_debug_get_scanline();
    *dot = (int)ppu_debug_get_scanline_clock();
}
static bool powered_before = false;

static bool power_on(char *rom_filename, const uint8_t *data, size_t size);

bool emu_power_on(char *rom_filename)
{
    return power_on(rom_filename, NULL, 0);
}

bool emu_power_on_memory(const uint8_t *data, size_t size, char *name)
{
    return power_on(name, data, size);
}

static bool power_on(char *rom_filename, const uint8_t *data, size_t size)
{
    if (cartridge_loaded) {
        mapper_deinit();
        cartridge_loaded = false;
    }
    sched_reset();
    log_set_clock(emulation_time);
    cpu_init();
    ppu_init();
    rp2a03_init();
    cpu_power_on();
    ppu_power_on();
    rp2a03_power_on();
    input_reset_lag_count(); // (a soft reset does not zero the lag counter, a power-on does)
    if (powered_before) {
        // Power cycle in a running process (debugger 'reset hard'): the PPU and APU timing state was set up by
        // the static initialisers the first time, so restart it the way the RESET button does (power-on timing:
        // scanline 0 / dot 0, even frame, fresh coroutine). The first power-on is unchanged.
        ppu_soft_reset();
        ppu_power_on();
        rp2a03_reset();
    }
    powered_before = true;
    if (true != (data != NULL ? mapper_init_memory(data, size, rom_filename) : mapper_init(rom_filename))) {
        LOG_ERROR(MAPPER, "cannot load %s", rom_filename);
        return false;
    }
    cartridge_loaded = true;
    cheats_set_game(rom_filename, mapper_rom_crc(), mapper_rom_sha1()); // its Game Genie codes (before cpu_reset())
    video_set_rom_path(rom_filename);                                   // screenshots are named after it
    if (data != NULL) {
        savestate_set_rom_data(rom_filename, data, size);
    } else {
        savestate_set_rom(rom_filename);
    } // (the slot files are named after the ROM too)
    script_set_rom(rom_filename); // emu.getRomInfo()
    fe_note_rom(rom_filename);    // the menu shows it
    menu_hint_game_started();     // (menu_hint.c: "Esc: menu" for a few seconds)
    if (cheats_active) {          // the player sees at once that the game runs with codes (instead of the hint)
        int n = cheats_active_count();
        video_overlay("Game Genie: %d code%s", n, n == 1 ? "" : "s");
    }
    cpu_reset();
    return true;
}

void emu_power_off(void)
{
    if (cartridge_loaded) {
        mapper_deinit();
        cartridge_loaded = false;
    }
    cpu_cleanup();
    ppu_cleanup();
    rp2a03_cleanup();
}
