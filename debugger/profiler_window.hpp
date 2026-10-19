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
// The profiler window (command wprof), after Mesen's Profiler: a window of its own (viewer_window.hpp) listing the
// routines with how often they were called and the cycles they took (profiler.hpp). Opening it starts the profiler,
// closing it stops it. The user sorts it with the keyboard:
//   1 Name  2 Address  3 Calls  4 Incl  5 Incl%  6 Excl  7 Excl%  8 Avg  9 Min  0 Max   (the same key again reverses)
//   Left / Right: the previous / next column      Up Down PageUp PageDown Home End: scroll
//   F: totals / per frame      R: reset the counts
#pragma once
#include "profiler.hpp"
#include "viewer_window.hpp"

class Symbols;

class ProfilerWindow : public ViewerWindow {
public:
    static constexpr int kWidth = 572, kHeight = 338;
    static constexpr int kRows = 28; // rows listed at once
    static ProfilerWindow &instance();

    void useSymbols(const Symbols *labels) { symbols = labels; } // the labels for the Name column

    // The state, also for the prof command and the tests
    Profiler::Column sortColumn() const { return column; }
    bool sortDescending() const { return descending; }
    void sortBy(Profiler::Column c); // as the keys: another column starts in its natural order, the same one reverses
    void toggleDirection() { descending = !descending; }

    // One line of the table: "name  address  calls  incl  incl%  excl  excl%  avg  min  max" in the window's columns
    // (PERFRAME: calls and cycles per frame). TOTAL is all the cycles profiled, for the percentages.
    static std::string line(const Profiler::Row &row, uint64_t total, uint64_t frames, bool perFrame);
    static std::string header();

private:
    ProfilerWindow();
    void render() override;
    std::string title() const override;
    void onKey(int key) override;
    void onKeyRepeat(int key) override;
    void onOpened() override;
    void onClosed() override;
    void scrollBy(int rows);

    const Symbols *symbols = nullptr;
    Profiler::Column column = Profiler::Column::Excl;
    bool descending = true;
    bool perFrame = false;
    int top = 0;
    size_t shown = 0;
};
