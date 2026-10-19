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
#include "gamegenie.hpp"
#include "core.hpp"
#include <optional>
#include <string>

// The Game Genie codec is in the core (cheats.c: the emulator applies codes too); this is its C++ face.

std::optional<GameGenieCode> gameGenieDecode(const std::string &text)
{
    cheat_code c;
    if (!cheats_decode(text.c_str(), &c)) {
        return std::nullopt;
    }
    GameGenieCode code{c.address, c.value, std::nullopt};
    if (c.has_compare) {
        code.compare = c.compare;
    }
    return code;
}

std::string gameGenieEncode(const GameGenieCode &code)
{
    cheat_code c{code.address, code.value, code.compare.has_value(), code.compare.value_or(0)};
    char text[9];
    cheats_encode(&c, text);
    return text;
}
