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
// Save states: the whole machine (CPU, APU, PPU, cartridge, the picture) in a file, to continue from later.
//
// When: a state is always taken at the same quiet moment, at an instruction boundary a few dots into scanline 240
// (the idle line after the picture). There the CPU, PPU and APU coroutines are each somewhere simple: the CPU
// between instructions, the PPU counting the dots of line 240, the APU at the top of its per-cycle loop. Their
// positions are therefore fully described by ordinary variables (scanline, dot...), and loading a state starts
// fresh coroutines that continue from there (cpu_state_restart(), ppu_state_restart()). Nothing of a coroutine's
// stack goes into the file, so states survive restarts and rebuilds of the same version. A save asked for at
// another moment waits for the next quiet moment (at most one frame).
//
// What: every module lists its variables in a state function (cpu_state(), ppu_state(), ...), which writes them
// on save and reads them back on load. tools/state_check.py (ctest savestate_fields) checks that every variable
// of the emulator core is listed, either with STATE() or as STATE_SKIP() (not part of the machine: a pointer to
// the ROM, a debugging switch, ...).
//
// The file: "ONESTATE", a format version, the CRC-32 of the ROM file, then the variables by name and size. A file
// is loaded only if the ROM matches and every variable is there with the same size (otherwise it was made by
// another version of ONE, and loading it is refused rather than risking a half-loaded machine).
#ifndef savestate_h_defined
#define savestate_h_defined

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct state_io state_io;

// One variable (or block of memory) of a module's state
void state_bytes(state_io *io, const char *name, void *data, size_t size);
bool state_loading(const state_io *io); // true when the values have just been loaded (for fix-ups)
#define STATE(io, var)  state_bytes((io), #var, &(var), sizeof(var))
#define STATE_SKIP(var) ((void)sizeof(var)) // not machine state; listed for tools/state_check.py

// The modules' state functions
void cpu_state(state_io *io); // also the APU (rp2a03.c is part of cpu.c)
void rp2a03_state(state_io *io);
void ppu_state(state_io *io);
void mapper_state(state_io *io);
void video_state(state_io *io);
void input_state(state_io *io); // the lag frame counter

// Restart the emulation coroutines from loaded variables (savestate.c, while loading)
void cpu_state_restart(bool boundary_hook_first);
void ppu_state_restart(void);
bool ppu_state_quiet(void); // the PPU is where a state can be taken (scanline 240, no deferred switch)

// --- for the emulator ------------------------------------------------------------------------------------------
void savestate_set_rom(const char *rom_path); // emu_power_on(): which game (the slot files and the ROM's CRC-32)
void savestate_set_rom_data(const char *rom_path, const uint8_t *data, size_t size); // the same, the file in memory

// At every instruction boundary (cpu.c): cheap test, then savestate_boundary() does any pending save or load
extern bool savestate_pending;
void savestate_boundary(void);

// Slots 1-9: files next to the ROM (game.nes -> game.ss1 ... game.ss9). The current slot starts at the slot
// saved most recently for the game. These report what happened with LOG_INFO(SAVESTATE) and video_overlay().
void savestate_save_slot(void); // at the next quiet moment (refused while the debugger holds the emulation)
void savestate_load_slot(void); // at the next instruction boundary, or at once if the emulation is not running
void savestate_change_slot(int delta);
int savestate_slot(void);
void savestate_set_slot(int slot);                                // (no message)
bool savestate_slot_time(int slot, char *text, size_t text_size); // when it was saved; false: empty
// States in memory (scripts): savestate_capture() asks for the state at the next point where one can be taken (a
// few PPU dots after the end of the picture, at the latest a frame later); done(data, size, context) then gets it,
// malloc'd (the callee frees it), or data NULL if it failed. savestate_load_memory() loads one at the next
// instruction (at once if the debugger has stopped the emulation); false: not a state of this ROM and version.
typedef void (*savestate_captured_fn)(uint8_t *data, size_t size, void *context);
void savestate_capture(savestate_captured_fn done, void *context);
bool savestate_load_memory(const uint8_t *data, size_t size);
uint32_t savestate_rom_crc(void);           // CRC32 of the loaded ROM file
void savestate_request_save_slot(int slot); // save to SLOT at the next quiet point (the debugger, stopped)
bool savestate_save_requested(void);        // a save is still waiting for its quiet point

// Command line: save to FILE at the first quiet moment of frame FRAME or later; load FILE before the first
// instruction (call after emu_power_on()). Return false (with a message on stderr) if the file is unusable.
void savestate_save_file_at_frame(const char *path, uint32_t frame);
bool savestate_load_file(const char *path);

// The libretro core (libretro/): retro_run() runs until the next quiet point and stops there, so that
// retro_serialize() / retro_unserialize() can work at once (savestate_save_buffer(), savestate_load_memory()).
// savestate_stop_at_quiet() asks the emulation to call HOOK (in the CPU coroutine, at an instruction boundary) at the
// first quiet point after the one it is at now (a whole frame later at most); HOOK gives control back to the host.
void savestate_stop_at_quiet(void (*hook)(void));
// PPU catch-up (FEATURE_PPU_CATCHUP): does a request need the PPU where the CPU is, now or very soon? A save asked for
// at a frame (--state-save --state-frame N) does not until it is within two frames of it (the CPU can be a whole frame
// ahead of the PPU); a load, a capture or a stop at the quiet point do at once.
bool savestate_wants_ppu(void);
// The state as savestate_capture() makes it, now: the machine must be at a quiet point (the host, right after the
// hook above; false if not). DATA is malloc'd (the caller frees it).
bool savestate_save_buffer(uint8_t **data, size_t *size);
// Bumped by every load: the debugger refreshes its windows when it changes
uint32_t savestate_load_count(void);

#endif
