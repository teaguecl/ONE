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
// The menu of the game window (C++, Dear ImGui). This is the only GUI header the emulator includes (one.c); the
// GUI itself sees the emulator only through frontend.h.
#ifndef gui_h_defined
#define gui_h_defined

#ifdef __cplusplus
extern "C" {
#endif

// Hands the menu to the emulator (fe_set_menu()): from then on the menu key opens it
void gui_install(void);

// For tests (gui_test.cpp): called once per frame of the menu, after drawing and before the frame is shown
// (FRAME counts from 0 each time the menu opens)
typedef void (*gui_frame_hook)(int frame);
void gui_set_frame_hook(gui_frame_hook hook);

#ifdef __cplusplus
}
#endif

#endif
