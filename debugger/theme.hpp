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
// Colors of the debugger, configurable in the config file (color.* keys). Independent of the user interface: the
// ImGui UI (imgui/views.hpp) turns a ThemeAttr into its colors.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "config.hpp"

enum class ThemeColor {
    // the debugger's own views
    Text,
    Value,
    Changed,
    RegisterChanged,
    Label,
    Dim,
    CurrentLine,
    BreakpointLine,
    CurrentBreakpointLine,
    CursorLine,
    CursorBreakpointLine,
    Running,
    // The desktop, window frames, menu bar and status line
    Desktop,
    FrameActive,
    FrameInactive,
    FrameIcons,
    Menu,
    MenuDisabled,
    MenuShortcut,
    MenuSelected,
    MenuSelectedDisabled,
    MenuSelectedShortcut,
    Count
};

// A color pair as the config file gives it: foreground and background as 0xRRGGBB, or the terminal's own color
// ('default'; the ImGui UI uses light gray on black for it), and the text style.
struct ThemeAttr {
    uint32_t fg = 0xAAAAAA, bg = 0x000000;
    bool fgDefault = false, bgDefault = false;
    bool bold = false, italic = false, underline = false;
};

class Theme {
public:
    enum class Preset { Borland, SoftIce };

    static Theme &instance();
    void usePreset(Preset preset);                                       // all colors from a built-in theme
    static bool presetFromName(const std::string &name, Preset &preset); // "borland", "softice"

    const ThemeAttr &operator[](ThemeColor c) const { return colors[static_cast<int>(c)]; }

    // Read the color.* keys; problems are added to messages.
    void apply(const Config &config, std::vector<std::string> &messages);
    static ThemeAttr fromBios(uint8_t attribute); // a BIOS attribute byte in the fixed VGA colors
    // A config file listing every key with its value in a built-in theme and a description (--write-config).
    static std::string defaultConfigText(Preset preset);

    // "white on blue", "#FFD700 on #000080 bold", "default on black", "0x1F". Returns false (and why) if invalid.
    static bool parse(const std::string &text, ThemeAttr &out, std::string &error);

private:
    Theme();
    ThemeAttr colors[static_cast<int>(ThemeColor::Count)]{};
};
