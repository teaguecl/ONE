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
// Watches: memory locations shown in the watch window (window 8) at every stop, SoftICE style. The list is kept in
// the ROM's name with .watch (game.nes -> game.watch) as watch commands, saved whenever it changes (UI only).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "symbols.hpp"

struct Watch {
    enum class Type { Byte, Word, Signed, Binary, Bytes };
    uint16_t address = 0;
    bool ppu = false; // PPU address space (watch -p)
    Type type = Type::Byte;
    int length = 1;   // bytes shown (Bytes: 1-16; Word: 2; others 1)
    std::string name; // given with the watch command; empty: the label at the address, if any

    std::vector<uint8_t> read() const;                   // the current bytes (no side effects)
    std::string typeText() const;                        // "b", "w", "s", "bin", "l 4"
    std::string addressText() const;                     // "$0774", "p$3F00" for PPU
    std::string valueText(const Symbols &symbols) const; // "$1F 31", "$A1B4 enemy_table", "%00101101", ...
    std::string label(const Symbols &symbols) const;     // name, else the label at the address
    std::string command() const;                         // the watch command that adds it again
};

class Watches {
public:
    static constexpr size_t kMax = 32;
    static Watches &instance();

    const std::vector<Watch> &list() const { return watches; }
    bool add(const Watch &watch); // false if the list is full
    bool remove(size_t index);
    void clear() { watches.clear(); }
    bool empty() const { return watches.empty(); }

    // The file the list is kept in (empty: not kept, e.g. --batch); CommandProcessor::loadWatches() reads it
    void setFileName(const std::string &file) { fileName = file; }
    const std::string &file() const { return fileName; }
    bool save() const; // an empty list removes the file

private:
    std::vector<Watch> watches;
    std::string fileName;
};
