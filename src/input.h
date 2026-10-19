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
#ifndef input_h_defined
#define input_h_defined

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "settings.h"

#define INPUT_PORTS   2 // NES controller ports
#define INPUT_BUTTONS 8 // A B Select Start Up Down Left Right

// Settings (one.cfg): the joyN.* and input.* keys. Applies the defaults first, so a file only needs the keys it
// changes; cfg may be NULL (defaults only). Errors are printed to stderr; returns how many.
// joyN.*, input.*, hotkey.* and menu.* in one.cfg (settings.h)
const setting_group *input_controller_settings(void);
const setting_group *input_hotkey_settings(void);

void input_init(void);         // after video_init(): gamepad support (nothing when headless)
void input_shutdown(void);     // before video_shutdown()
int input_list_gamepads(void); // one --list-gamepads: list the pads, print button names as they are pressed

void input_poll(void);
void input_wait(void);
// one-debugger's own window: HOOK sees every SDL event (an SDL_Event *) before the game window does, and returns
// true for the events it took (those of its window). NULL removes it.
typedef bool (*input_event_hook)(void *sdl_event, void *context);
void input_set_event_hook(input_event_hook hook, void *context);
uint8_t input_joy1_read(void);
uint8_t input_joy2_read(void);
void input_set_scripted_start(uint64_t cpu_cycle); // hold START for 2 frames from this CPU cycle (-S)
bool input_load_script(const char *path);
// The Zapper on port 2 as the script ("CYCLE zapper X Y [fire]" lines) sets it at this CPU cycle: aimed at NES pixel
// X, Y (on_screen false: away from the screen), trigger pulled. Returns false when the script has no zapper lines.
typedef struct {
    bool on_screen;
    int x, y;
    bool trigger;
} input_zapper_state;
bool input_scripted_zapper(input_zapper_state *state);
bool input_zapper_present(void); // a Zapper is on port 2 (input.zapper, a script's zapper lines, a frontend)
bool input_zapper_aim(input_zapper_state *state);       // where it points now: script, else frontend, else the mouse
void input_set_zapper(const input_zapper_state *state); // a frontend's light gun (NULL: none); libretro, the debugger
void input_set_zapper_frame(const input_zapper_state *state); // a script's aim until the next frame (NULL: none)
void input_zapper_pull_trigger(
    uint64_t until_cycle); // hold the trigger of input_set_zapper()'s aim until this CPU cycle
// "a+right", "start", "-" (none): the buttons by name (a b select start up down left right)
bool input_parse_buttons(const char *text, uint8_t *buttons);
// Hold BUTTONS on PORT (0, 1) until CPU cycle UNTIL_CYCLE, on top of the keyboard/gamepad (the debugger's 'press')
void input_hold(int port, uint8_t buttons,
                uint64_t until_cycle); // --input FILE: buttons by CPU cycle (regression/input_from_log.py)

// Scripts (script.c). Buttons are a bit mask: bit 0 A, 1 B, 2 Select, 3 Start, 4 Up, 5 Down, 6 Left, 7 Right.
uint8_t input_buttons(int port);                    // what the game would read now (a script's override included)
void input_set_override(int port, uint8_t buttons); // emu.setInput(): these buttons until the next frame's poll
void input_clear_overrides(void);                   // (each frame, before the inputPolled event)

// The lag frame counter (FCEUX's): a lag frame is a frame in which the game did not poll the controllers (a strobe
// that latches them, or a read of $4016 / $4017). Counted by video_update_screen() at the end of every picture,
// always (not only while a script runs); it is part of a save state. Power-on zeroes it, a soft reset does not.
void input_note_poll(void);               // the game read a controller port
void input_frame_done(void);              // the picture is complete: count the frame if it was a lag frame
uint32_t input_lag_count(void);           // lag frames so far
bool input_last_frame_lagged(void);       // the frame that has just ended was one
void input_reset_lag_count(void);         // (power-on; the debugger's lag reset)
bool input_four_score(void);              // input.four_score: a Four Score adapter instead of two controllers
bool input_key_pressed(const char *name); // emu.isKeyPressed(): a key by SDL name ("A", "Space", "F1")

#endif
