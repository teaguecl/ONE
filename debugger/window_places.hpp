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
// Where one-debugger's windows were when it last quit -- the game window, and the ImGui UI's own window -- so that
// they open there again. Kept in $XDG_STATE_HOME/one/window-places.txt (~/.local/state/one/, next to recent.txt).
#pragma once
#include <string>

struct SDL_Window;

struct WindowPlace {
    bool known = false; // (false: none saved, or not on any screen now)
    int x = 0, y = 0, w = 0, h = 0;
    bool maximized = false;
};

// The place saved under NAME ("game", "debugger"); not known if there is none or it is off every screen now
WindowPlace savedWindowPlace(const std::string &name);
// Remember where WINDOW is now, under NAME (the other names' places are kept); nothing for a full-screen window
void saveWindowPlace(const std::string &name, SDL_Window *window);
// Put WINDOW where PLACE says (if it is known)
void moveWindowTo(SDL_Window *window, const WindowPlace &place);
