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
// The background viewer (command wbg): a window of its own showing the four nametables (512 x 480: $2000 $2400 /
// $2800 $2C00, through the cartridge's mirroring) drawn with the background pattern table and palettes as they are
// now, and the part the game window shows: each picture line's 256 pixels, from the scroll it started with
// (ppu_debug_line_scroll()), at full brightness with a white outline; the rest dimmed. O in the window switches the
// outline to one 256 x 240 rectangle from the scroll the picture starts with (as Mesen draws it), and back. It is
// redrawn at every stop and, while the game runs, once a frame (viewer_window.hpp). Both user interfaces use it.
#pragma once
#include "viewer_window.hpp"

class BackgroundViewer : public ViewerWindow {
public:
    static constexpr int kWidth = 512, kHeight = 480;
    static BackgroundViewer &instance();

private:
    BackgroundViewer();
    void render() override;
    std::string title() const override;
    void onKey(int key) override;
    bool perLine = true; // the part shown: each line where its own scroll put it; false: one rectangle (O)
};
