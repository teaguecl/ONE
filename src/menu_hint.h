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
// Telling players that the game window has a menu (frontend.h, gui/), which has no menu bar to show it: when a
// game starts, "Esc: menu" at the bottom of the picture for a few seconds (menu.hint, on by default).
// All of it is in menu_hint.c; the rest of the emulator only calls menu_hint_game_started() (emu.c) and lists
// menu_hint_settings() in settings.c. Builds without a menu (one-debugger, -DONE_BUILD_GUI=OFF) do not show it.
#ifndef menu_hint_h_defined
#define menu_hint_h_defined

#include "settings.h"

const setting_group *menu_hint_settings(void); // menu.hint

void menu_hint_game_started(void); // emu_power_on(): the start-up hint

#endif
