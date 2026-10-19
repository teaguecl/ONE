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
// What the game window's menu (gui/, C++ with Dear ImGui) may use of the emulator: this header (and settings.h,
// which it includes) is the whole interface between the two. The GUI includes no other header of the core, and
// the core includes none of the GUI's except gui/gui.h in one.c (checked by tools/separation_check.py, ctest
// gui_separation). Builds without the GUI (one-debugger, -DONE_BUILD_GUI=OFF, headless) have no menu: the menu key
// does nothing.
//
// The menu runs while the emulation waits: input_poll() (once a frame, inside the emulation) calls the menu
// function, which keeps the window busy until the menu closes and returns what to do next. Pointers to SDL objects
// are passed as void * so that this header does not need SDL.
#ifndef frontend_h_defined
#define frontend_h_defined

#include <stdbool.h>
#include <stddef.h>
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

// What the emulator does when the menu closes
typedef enum {
    FE_RESUME,      // carry on
    FE_QUIT,        // end the program
    FE_RESET,       // press RESET
    FE_POWER_CYCLE, // switch off and on again (the same game)
    FE_OPEN_ROM,    // switch off and start the game fe_set_next_rom() named
    FE_SAVE_STATE,  // save to the current slot
    FE_LOAD_STATE,  // load from the current slot
} fe_action;

// The menu. GAME_RUNNING false: no game yet (one started without a ROM): the menu offers to open one.
typedef fe_action (*fe_menu_fn)(bool game_running);
void fe_set_menu(fe_menu_fn fn); // gui_install() (one.c) sets it; NULL = no menu
bool fe_menu_installed(void);    // there is a menu (in this build)
bool fe_menu_available(void);    // ... and a window to show it in
const char *fe_version(void);    // ONE's version (version.h), for the menu to show

// --- for the menu -------------------------------------------------------------------------------------------------

// The window: SDL_Window * and SDL_Renderer * (NULL when headless)
void *fe_sdl_window(void);
void *fe_sdl_renderer(void);

// Draw the last frame of the game into the whole window, keeping its shape (black bars), without presenting
// it: the menu draws over it. (Black when no game has run yet.)
void fe_draw_game(void);

// An SDL event (SDL_Event *) that arrived while the menu is open, for the emulator's own business: the window
// being closed (returns true: the program should end) and gamepads being connected or disconnected.
bool fe_window_event(const void *sdl_event);

// The name of the input an SDL event presses, as one.cfg writes it: "key:period", "pad1:a", "pad1:leftx-",
// "pad2:button5", "pad1:hat0_up"... KEYS_ONLY (hotkeys): keys only, with the modifiers held: "key:shift+f12".
// Returns false if the event presses nothing nameable (a key or button let go, a modifier key alone, a stick
// still near its centre).
bool fe_input_name(const void *sdl_event, bool keys_only, char *name, size_t name_size);

// The menu's own key (hotkey.menu) or gamepad button (menu.gamepad_button): true if this event presses it
bool fe_is_menu_button(const void *sdl_event);

// The game
const char *fe_rom_path(void);          // "" before any game was started
void fe_set_next_rom(const char *path); // for FE_OPEN_ROM
const char *fe_next_rom(void);          // (one.c)

// Recently opened games and Lua scripts (full paths, newest first; kept in ~/.local/state/one/)
#define FE_RECENT_MAX 10
typedef enum { FE_RECENT_GAMES, FE_RECENT_SCRIPTS } fe_recent_list;
int fe_recent_count(fe_recent_list which);
const char *fe_recent(fe_recent_list which, int index); // 0 = the newest
void fe_recent_clear(fe_recent_list which);
void fe_recent_add(fe_recent_list which, const char *path); // (one adds what the menu opens; the debugger its own)

// Lua scripts (script.h): the running ones, starting one (it goes into the recent list; false: the error is in
// fe_script_error()), stopping one (its drawings go too)
int fe_script_count(void);
const char *fe_script_name(int index);
const char *fe_script_path(int index);
bool fe_script_load(const char *path);
bool fe_script_stop(int index);
const char *fe_script_error(void); // the last error of a script, at load or later ("" if none)

// Save state slots 1-9 (files next to the ROM)
int fe_slot(void);
void fe_set_slot(int slot);
bool fe_slot_time(int slot, char *text, size_t text_size); // "2026-10-03 14:22", false if the slot is empty

// Game Genie codes for the running game (cheats.h): the game's list (codes ticked or not, remembered per game),
// Game Genie on/off, and the known codes for the game (the compiled-in database)
const char *fe_gg_game(void); // the game as the database knows it ("" when it does not)
const char *fe_gg_file(void); // where the list is kept ("" when it is not)
bool fe_gg_enabled(void);
void fe_gg_set_enabled(bool on);
int fe_gg_count(void); // the game's list
bool fe_gg_entry(int index, char *code, size_t code_size, char *description, size_t description_size, bool *on);
void fe_gg_set_on(int index, bool on); // (ticking switches Game Genie on)
void fe_gg_delete(int index);
// A code as typed: what it does ("$C5F6 = 42 if it reads 00"); false if it is not a code (TEXT says why)
bool fe_gg_describe(const char *code, char *text, size_t text_size);
bool fe_gg_add(const char *code, const char *description); // into the list, ticked; false: not a code, or full
int fe_gg_db_count(void);                                  // the known codes
bool fe_gg_db_entry(int index, char *codes, size_t codes_size, char *description, size_t description_size);
int fe_gg_db_state(int index);         // 0: not in the list, 1: in it but not all ticked, 2: in it and ticked
void fe_gg_db_set(int index, bool on); // tick: into the list (ticked); untick: its codes unticked (kept listed)

// Where log messages go: the --log file's name, or "stderr" / "stdout"
const char *fe_log_destination(void);

// Screenshot of the last frame (next to the ROM, numbered). SAVED gets the file name.
bool fe_screenshot(char *saved, size_t saved_size);

// FPS counter in the window title
bool fe_fps_shown(void);
void fe_set_fps(bool show);
// The lag counter on the picture
bool fe_lag_counter_shown(void);
void fe_set_lag_counter(bool show);

// --- for the emulator (input.c, emu.c, one.c) ---------------------------------------------------------------------

// Open the menu (if there is one and a window): sound paused meanwhile, changed settings saved when it closes
fe_action fe_run_menu(bool game_running);
void fe_note_rom(const char *path); // emu_power_on(): the game now running

#ifdef __cplusplus
}
#endif

#endif
