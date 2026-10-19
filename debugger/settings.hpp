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
// General (non-color) settings of the config file. Colors are in theme.hpp.
#pragma once
#include <string>
#include <vector>
#include "config.hpp"

struct Settings {
    bool shadows = false;       // drop shadows under the windows
    std::string handbackButton; // a gamepad button that stops the game, as Ctrl+D does ("": none; SDL's names)
};

// The settings in effect (defaults, then the config file)
Settings &settings();

// Read the settings' keys from the config file; problems are added to messages.
void applySettings(const Config &config, std::vector<std::string> &messages);

// An SDL event: the gamepad's hand-back button pressed (handback_button; the MCP adapter's wait_for_user)
bool isHandbackButton(const void *sdlEvent);

// "key = default   # description" lines for every setting (--write-config)
std::string settingsConfigText();
