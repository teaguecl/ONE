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
// NES Game Genie codes: 6 letters (address, value) or 8 letters (address, value, compare). Encoding and decoding
// (the codec is the core's, cheats.c); the core applies the codes that are on ('gg on').
#pragma once
#include <cstdint>
#include <optional>
#include <string>

struct GameGenieCode {
    uint16_t address = 0;           // $8000-$FFFF: the CPU read of this ROM address is replaced
    uint8_t value = 0;              // ... by this byte
    std::optional<uint8_t> compare; // 8-letter codes: only when the ROM byte is this (bank-switched games)
};

std::optional<GameGenieCode> gameGenieDecode(const std::string &text); // nullptr-like if not a valid code
std::string gameGenieEncode(const GameGenieCode &code);
