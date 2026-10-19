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
// Lua scripting in the style of Mesen 2 (script.c): scripts register callbacks with emu.addEventCallback() and use
// emu.read(), emu.write(), emu.getState()... with Mesen's names (emu.memType.nesMemory, emu.eventType.endFrame).
// Each script has a Lua state of its own. Without a script loaded, the emulator pays one predictable branch at each
// event site (script_active is false), so 'one' runs at full speed.
#ifndef script_h_defined
#define script_h_defined

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Mesen 2's emu.eventType, in the same order
typedef enum {
    SCRIPT_EVENT_RESET,        // power-on / RESET button: after the CPU read the reset vector
    SCRIPT_EVENT_NMI,          // an NMI was taken (PC = the handler)
    SCRIPT_EVENT_IRQ,          // an IRQ was taken (PC = the handler)
    SCRIPT_EVENT_START_FRAME,  // the PPU starts a frame (pre-render line, dot 0)
    SCRIPT_EVENT_END_FRAME,    // the picture is done (where Mesen's endFrame sees the CPU cycle counter)
    SCRIPT_EVENT_INPUT_POLLED, // the controllers were read from the keyboard / gamepads for this frame
    SCRIPT_EVENT_STATE_LOADED, // a save state was loaded
    SCRIPT_EVENT_STATE_SAVED,  // a save state was saved
    SCRIPT_EVENT_SCRIPT_ENDED, // (only to the script that ends)
    SCRIPT_EVENT_CODE_BREAK,   // the debugger stopped
    SCRIPT_EVENTS
} script_event_type;

extern bool script_active; // a script is loaded

bool script_load(const char *path);  // run a script (its main chunk registers callbacks); false: error logged
void script_stop_all(void);          // end every script (scriptEnded callbacks first)
bool script_in_callback(void);       // Lua code is running (scripts cannot be loaded or stopped from there)
bool script_stop(int index);         // end one script (false: no such script, or Lua code is running)
const char *script_path(int index);  // the file a loaded script came from
const char *script_last_error(void); // the last error of any script ("" if none since script_clear_error())
void script_clear_error(void);
int script_count(void);
const char *script_name(int index);    // file name of a loaded script
int script_exit_code(void);            // set by emu.stop(code); 0 if no script asked to stop
void script_set_rom(const char *path); // emu_power_on(): the ROM emu.getRomInfo() describes
// emu.stop(code): by default the emulation stops (one exits with code); the debugger installs its own
void script_set_stop_hook(void (*hook)(int code));

// one-debugger: debugger.command(line) runs a debugger command and returns its output (malloc'd, freed by the
// caller); emu.breakExecution() stops at the next instruction; emu.getLabelAddress(label) asks for a label's CPU
// address (-1: unknown). Set before scripts are loaded: only scripts loaded afterwards have these functions.
void script_set_debugger_hooks(char *(*command)(const char *line), void (*break_execution)(void),
                               int (*label_address)(const char *label));

// Breakpoint conditions (one-debugger): a Lua expression, compiled once (-1: error, described in ERROR) and evaluated
// when the breakpoint is reached. It sees the registers (a, x, y, s, p, pc), scanline, cycle, frame, mem[address],
// the emu functions, and ADDRESS and VALUE of the access (memory breakpoints; -1 otherwise). An error counts as true.
int script_condition_compile(const char *expression, char *error, size_t error_size);
bool script_condition_eval(int handle, int address, int value);
void script_condition_free(int handle);

// video_update_screen(): a frame is complete. Draws the scripts' drawings for it into the layer over the picture
// and counts down their durations; also notes whether the frame was a lag frame.
void script_frame_done(void);

// Memory callbacks (emu.addMemoryCallback): the CPU calls these only for addresses a script watches. The flags are
// false while no script watches anything, so the emulation pays one predictable branch per access.
extern bool script_hook_read, script_hook_write, script_hook_exec;
extern uint8_t script_watch[0x10000];                      // per CPU address: bit 0 read, bit 1 write, bit 2 exec
uint8_t script_mem_read(uint16_t address, uint8_t value);  // returns the value the CPU reads
uint8_t script_mem_write(uint16_t address, uint8_t value); // returns the value written
void script_mem_exec(uint16_t pc);                         // an instruction is about to run at pc
#define SCRIPT_READ(address, value)                                                                                    \
    do {                                                                                                               \
        if (script_hook_read && (script_watch[(uint16_t)(address)] & 1))                                               \
            (value) = script_mem_read((uint16_t)(address), (value));                                                   \
    } while (0)
#define SCRIPT_WRITE(address, value)                                                                                   \
    do {                                                                                                               \
        if (script_hook_write && (script_watch[(uint16_t)(address)] & 2))                                              \
            (value) = script_mem_write((uint16_t)(address), (value));                                                  \
    } while (0)

void script_event_fire(script_event_type type);
static inline void script_event(script_event_type type)
{
    if (script_active) {
        script_event_fire(type);
    }
}

#endif
