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
// The palette viewer (command wpal), after Mesen's Palette Viewer: a window of its own (viewer_window.hpp) showing
// palette RAM -- BG0-BG3 and SP0-SP3, four colors each, with their values; the entries that mirror the backdrop
// ($3F10 $3F14 $3F18 $3F1C) marked in a corner -- and the NES's 64 colors, the ones palette RAM uses outlined. The
// line at the bottom (and the title) says what the mouse is over. lua/examples/palette.lua draws the same picture.
#pragma once
#include "viewer_window.hpp"

class PaletteViewer : public ViewerWindow {
public:
    static constexpr int kWidth = 280, kHeight = 218;
    static PaletteViewer &instance();

private:
    PaletteViewer();
    void render() override;
    std::string title() const override;
    void onMouse(int x, int y) override;
    std::string hoverText() const; // what the mouse is over ("" when nothing)
    int mouseX = -1, mouseY = -1;
};
