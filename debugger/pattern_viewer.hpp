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
// The pattern table viewer (command wpat), after Mesen's Tile Viewer: a window of its own (viewer_window.hpp) showing
// both pattern tables side by side ($0000 left, $1000 right, a line between), 16 x 16 tiles each, through the
// cartridge's CHR banks as they are now, in one of the palettes (P: BG0-BG3, SP0-SP3, gray; BG0 to start with); G
// lays a tile grid over them. The labels say which table PPUCTRL uses for the background (BG) and for sprites (SP).
// The line at the bottom (and the title) says which tile the mouse is over. lua/examples/pattern.lua draws the same
// picture.
#pragma once
#include "viewer_window.hpp"

class PatternViewer : public ViewerWindow {
public:
    static constexpr int kWidth = 273, kHeight = 157;
    static PatternViewer &instance();

private:
    PatternViewer();
    void render() override;
    std::string title() const override;
    void onKey(int key) override;
    void onMouse(int x, int y) override;
    std::string hoverText() const; // the tile the mouse is over ("" when none)
    int palette = 0;               // 0-3 BG0-BG3, 4-7 SP0-SP3, 8 gray (P)
    bool grid = false;             // (G)
    int mouseX = -1, mouseY = -1;
};
