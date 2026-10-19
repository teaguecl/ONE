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
// The sprites in OAM, decoded (the OAM window and the oam command): position, tile, palette, priority, flips, the
// picture lines each covers, and what goes wrong -- lines with more than 8 sprites (the PPU drops the rest), sprite 0
// and its hit, sprites hidden below the picture or under the left-column mask.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct OamSprite {
    int index = 0;
    uint8_t y = 0, tile = 0, attr = 0, x = 0; // the four OAM bytes
    int top = 0, bottom = 0;                  // the picture lines it covers (top = Y + 1)
    bool hidden = false;                      // below the picture (Y >= $EF)
    int droppedFirst = -1, droppedLast = -1;  // lines where it is the 9th or later sprite (not drawn there)
    bool leftMasked = false;                  // X < 8 while the PPU hides sprites in the left 8 pixels
    int palette() const { return attr & 3; }
    bool behind() const { return (attr & 0x20) != 0; }
};

struct OamReport {
    std::array<OamSprite, 64> sprites;
    bool tall = false;  // 8x16 sprites
    uint16_t table = 0; // 8x8: the sprites' pattern table ($0000 / $1000)
    bool spritesOn = true;
    bool leftShown = true; // sprites in the left 8 pixels
    int visible = 0, dropped = 0;
    int busiestLine = -1, busiestCount = 0;
    struct Overflow {
        int first, last, most; // lines first..last have more than 8 sprites, at most MOST
    };
    std::vector<Overflow> overflows;
    bool dmaSeen = false;
    uint8_t dmaPage = 0;
    uint32_t dmaFrame = 0;
    bool hitSeen = false;
    int hitLine = -1, hitX = -1;
    uint32_t hitFrame = 0;
    uint32_t frame = 0; // now
};

OamReport decodeOam();
std::string oamColumnTitles();
std::vector<std::string> oamHeader(const OamReport &report); // two lines: the setup, then the warnings
std::string oamRow(const OamReport &report, int index);      // one sprite (columns as oamColumnTitles())
std::string oamNotes(const OamReport &report, int index);    // its notes column ("" if nothing to say)
