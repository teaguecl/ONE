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
// Windows for Lua scripts (one.openWindow() and the rest, script.c): each a window of its own, beside the game
// window, showing a picture the script sets. Without a window to show (headless, --batch, no SDL2) a window still
// exists as a picture, so one.saveWindow() works (tests).
#ifndef script_window_h_defined
#define script_window_h_defined

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCRIPT_WINDOW_MAX_KEYS 16 // keys queued for one.windowKeys() between two calls

// A new window (OWNER: the script it belongs to) of WIDTH x HEIGHT pixels, shown SCALE times that size (0: twice
// if that fits on the screen, else once; the user can resize it). Returns its id (> 0), or 0 and why in ERROR.
int script_window_open(const void *owner, const char *title, int width, int height, double scale, char *error,
                       size_t error_size);
bool script_window_exists(const void *owner, int id); // (false: not the owner's, or closed by the program)
bool script_window_closed_by_user(const void *owner, int id);
void script_window_size(const void *owner, int id, int *width, int *height);
uint32_t *script_window_pixels(const void *owner, int id); // its picture: width x height 0x00RRGGBB, to be filled
void script_window_show(const void *owner, int id);        // show what was put into the picture
void script_window_close(const void *owner, int id);
void script_window_close_all(const void *owner); // (the script ended)
// The keys pressed in the window since the last call (SDL key names: "O", "Space", "F1"); -1 if the user closed it
int script_window_keys(const void *owner, int id, const char *names[SCRIPT_WINDOW_MAX_KEYS]);
bool script_window_save(const void *owner, int id, const char *path); // the picture as a PNG
// The mouse over the window's picture: X, Y in its pixels, BUTTONS (1 left, 2 middle, 4 right); false: not over it
bool script_window_mouse(const void *owner, int id, int *x, int *y, unsigned *buttons);
bool script_windows_shown(void); // windows can be seen (SDL video is there and the program is not headless)

// input.c: an SDL event (SDL_Event *) for one of these windows -- handled (its close button, its keys, redrawing).
// false: another window's.
bool script_window_event(const void *sdl_event);

#endif
