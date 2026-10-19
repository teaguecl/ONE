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
#include "theme.hpp"
#include "debugger/config.hpp"
#include "settings.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Entry {
    ThemeColor color;
    const char *key;
    const char *borland; // default in the "borland" theme (Borland IDE blue)
    const char *softice; // default in the "softice" theme (SoftICE COLOR defaults: 07 0F 71 30 02)
    int paletteIndex;    // > 0: a desktop / frame / menu color, listed in its own section of the config file
    const char *description;
};

// SoftICE's COLOR command has five attributes: normal 07 (light gray on black), bold 0F (white on black),
// reverse 71 (blue on light gray), help line 30 (black on cyan) and window lines 02 (green on black).
// (not reformatted: a table; docs/quickref/extract.sh reads it line by line)
// clang-format off
const Entry kEntries[] = {
    {ThemeColor::Text,                  "color.text",                    "lightgray on blue",     "lightgray on black",  0, "normal text in all windows"},
    {ThemeColor::Value,                 "color.value",                   "white on blue",         "lightgray on black",  0, "register values, mnemonics, commands echoed in the command window"},
    {ThemeColor::Changed,               "color.changed",                 "yellow on blue",        "white on black",      0, "values that changed since the last stop"},
    {ThemeColor::RegisterChanged,       "color.register_changed",        "yellow on blue",        "cyan on black",       0, "registers and flags that changed (all of PC=1234)"},
    {ThemeColor::Label,                 "color.label",                   "lightcyan on blue",     "white on black",      0, "labels and instruction comments"},
    {ThemeColor::Dim,                   "color.dim",                     "cyan on blue",          "lightgray on black",  0, "addresses, register names, instruction bytes, clear flags"},
    {ThemeColor::CurrentLine,           "color.current_line",            "black on cyan",         "blue on lightgray",   0, "code window: the instruction at PC"},
    {ThemeColor::BreakpointLine,        "color.breakpoint_line",         "white on red",          "white on black",      0, "code window: an instruction with a breakpoint"},
    {ThemeColor::CurrentBreakpointLine, "color.current_breakpoint_line", "yellow on red",         "black on lightgray",  0, "code window: the instruction at PC, on a breakpoint"},
    {ThemeColor::CursorLine,            "color.cursor_line",             "blue on lightgray",     "black on cyan",       0, "code window: the cursor line (when the window has the focus)"},
    {ThemeColor::CursorBreakpointLine,  "color.cursor_breakpoint_line",  "red on lightgray",      "white on darkcyan",   0, "code window: the cursor line, on a breakpoint"},
    {ThemeColor::Running,               "color.running",                 "yellow on blue",        "white on black",      0, "command window: the 'Running...' line"},
    {ThemeColor::Desktop,               "color.desktop",                 "blue on lightgray",     "black on black",      1, "desktop background"},
    {ThemeColor::FrameActive,           "color.frame_active",            "white on blue",         "lightgreen on black", 9, "frame and title of the active window"},
    {ThemeColor::FrameInactive,         "color.frame_inactive",          "lightgray on blue",     "green on black",      8, "frame and title of the other windows"},
    {ThemeColor::FrameIcons,            "color.frame_icons",             "lightgreen on blue",    "lightgreen on black", 10, "close/zoom icons of the active window"},
    {ThemeColor::Menu,                  "color.menu",                    "black on lightgray",    "black on cyan",       2, "menu bar, menus and status line"},
    {ThemeColor::MenuDisabled,          "color.menu_disabled",           "darkgray on lightgray", "darkgray on cyan",    3, "disabled menu items"},
    {ThemeColor::MenuShortcut,          "color.menu_shortcut",           "red on lightgray",      "white on cyan",       4, "highlighted letters and keys in menus and the status line"},
    {ThemeColor::MenuSelected,          "color.menu_selected",           "black on green",        "white on black",      5, "selected menu item"},
    {ThemeColor::MenuSelectedDisabled,  "color.menu_selected_disabled",  "darkgray on green",     "darkgray on black",   6, "selected disabled menu item"},
    {ThemeColor::MenuSelectedShortcut,  "color.menu_selected_shortcut",  "red on green",          "lightgreen on black", 7, "highlighted letter of the selected menu item"},
};
// clang-format on

const char *presetValue(const Entry &e, Theme::Preset preset)
{
    return (preset == Theme::Preset::SoftIce) ? e.softice : e.borland;
}

const char *const kColorNames[16] = {
    "black",    "blue",      "green",      "cyan",      "red",      "magenta",      "brown",  "lightgray",
    "darkgray", "lightblue", "lightgreen", "lightcyan", "lightred", "lightmagenta", "yellow", "white",
};

// The color names are fixed RGB values -- the standard VGA text-mode palette, as DOS and SoftICE showed them --
// so that the debugger looks the same in every terminal, whatever palette the terminal uses for its 16 colors.
const uint32_t kVgaRgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

// Extra names beyond the 16 VGA colors (CSS/X11 names and values)
const struct {
    const char *name;
    uint32_t rgb;
} kExtraColors[] = {
    {"darkcyan", 0x008B8B},
};

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// A color name, #RRGGBB or "default"; isDefault: the terminal's own color
bool parseColor(const std::string &word, uint32_t &rgb, bool &isDefault)
{
    std::string w = lower(word);
    if (w == "lightgrey" || w == "gray" || w == "grey")
        w = "lightgray";
    if (w == "darkgrey")
        w = "darkgray";
    isDefault = false;
    for (int i = 0; i < 16; i++) {
        if (w == kColorNames[i]) {
            rgb = kVgaRgb[i];
            return true;
        }
    }
    for (const auto &extra : kExtraColors) {
        if (w == extra.name) {
            rgb = extra.rgb;
            return true;
        }
    }
    if (w == "default") {
        isDefault = true;
        return true;
    }
    if (w.size() == 7 && w[0] == '#') {
        char *end = nullptr;
        unsigned long value = std::strtoul(w.c_str() + 1, &end, 16);
        if (*end == '\0') {
            rgb = static_cast<uint32_t>(value);
            return true;
        }
    }
    return false;
}

} // namespace

Theme &Theme::instance()
{
    static Theme theme;
    return theme;
}

Theme::Theme()
{
    usePreset(Preset::SoftIce);
}

void Theme::usePreset(Preset preset)
{
    for (const Entry &e : kEntries) {
        std::string error;
        parse(presetValue(e, preset), colors[static_cast<int>(e.color)], error);
    }
}

bool Theme::presetFromName(const std::string &name, Preset &preset)
{
    std::string n = lower(name);
    if (n == "borland") {
        preset = Preset::Borland;
    } else if (n == "softice") {
        preset = Preset::SoftIce;
    } else {
        return false;
    }
    return true;
}

bool Theme::parse(const std::string &text, ThemeAttr &out, std::string &error)
{
    std::istringstream in(text);
    std::vector<std::string> words;
    for (std::string w; in >> w;) {
        words.push_back(w);
    }
    if (words.size() == 1 && lower(words[0]).rfind("0x", 0) == 0) { // BIOS attribute byte: background << 4 | fg
        char *end = nullptr;
        unsigned long value = std::strtoul(words[0].c_str() + 2, &end, 16);
        if (*end != '\0' || value > 0xFF || words[0].size() < 3) {
            error = "'" + words[0] + "' is not a byte in hex (0x00-0xFF)";
            return false;
        }
        out = fromBios(static_cast<uint8_t>(value));
        return true;
    }
    if (words.size() < 3 || lower(words[1]) != "on") {
        error = "expected 'foreground on background [bold] [italic] [underline]'";
        return false;
    }
    ThemeAttr attr;
    if (!parseColor(words[0], attr.fg, attr.fgDefault)) {
        error = "unknown color '" + words[0] + "'";
        return false;
    }
    if (!parseColor(words[2], attr.bg, attr.bgDefault)) {
        error = "unknown color '" + words[2] + "'";
        return false;
    }
    for (size_t i = 3; i < words.size(); i++) {
        std::string s = lower(words[i]);
        if (s == "bold")
            attr.bold = true;
        else if (s == "italic")
            attr.italic = true;
        else if (s == "underline")
            attr.underline = true;
        else {
            error = "unknown style '" + words[i] + "' (bold, italic, underline)";
            return false;
        }
    }
    out = attr;
    return true;
}

void Theme::apply(const Config &config, std::vector<std::string> &messages)
{
    for (const Entry &e : kEntries) {
        const std::string *value = config.get(e.key);
        if (value == nullptr) {
            continue;
        }
        std::string error;
        ThemeAttr attr;
        if (parse(*value, attr, error)) {
            colors[static_cast<int>(e.color)] = attr;
        } else {
            messages.push_back(config.path() + ":" + std::to_string(config.line(e.key)) + ": " + e.key + ": " + error);
        }
    }
}

ThemeAttr Theme::fromBios(uint8_t attribute)
{
    ThemeAttr attr;
    attr.fg = kVgaRgb[attribute & 0x0F];
    attr.bg = kVgaRgb[(attribute >> 4) & 0x0F];
    return attr;
}

std::string Theme::defaultConfigText(Preset preset)
{
    std::ostringstream out;
    out << "# one-debugger configuration -- "
        << (preset == Preset::SoftIce ? "SoftICE colors (one-debugger --write-config FILE --theme softice)\n"
                                      : "Borland colors (one-debugger --write-config FILE --theme borland)\n")
        << "#\n"
           "# One setting per line: key = value.  A line starting with '#', or ' # ' after a value, is a comment.\n"
           "# The debugger reads --config FILE, or else ./one-debugger.cfg, or else ~/.config/one/debugger.cfg\n"
           "# ($XDG_CONFIG_HOME/one/debugger.cfg).  Settings that are left out come from the built-in theme:\n"
           "# softice (the default), or borland with --theme borland.\n"
           "#\n"
           "# Colors: 'foreground on background', optionally followed by bold, italic and/or underline.\n"
           "#   Color names: black blue green cyan red magenta brown lightgray\n"
           "#                darkgray lightblue lightgreen lightcyan lightred lightmagenta yellow white\n"
           "#                darkcyan (#008B8B)\n"
           "#   (the VGA text-mode colors: black = #000000, blue = #0000AA, lightgray = #AAAAAA ..., the same in\n"
           "#   every terminal), or #RRGGBB, or 'default' (the terminal's own color).  A terminal without 24-bit\n"
           "#   color shows the nearest color it has.  A BIOS attribute byte (0x1F = white on blue) works too.\n";
    bool tvSection = false;
    out << "\ntheme = " << (preset == Preset::SoftIce ? "softice" : "borland")
        << "                                     # built-in colors for anything not set below\n";
    out << "\n# --- windows ---\n" << settingsConfigText();
    out << "\n# --- colors of the debugger windows ---\n";
    for (const Entry &e : kEntries) {
        if (e.paletteIndex > 0 && !tvSection) {
            out << "\n# --- desktop, window frames, menu bar and status line ---\n";
            tvSection = true;
        }
        std::string line = std::string(e.key) + " = " + presetValue(e, preset);
        out << line << std::string(line.size() < 52 ? 52 - line.size() : 1, ' ') << "# " << e.description << "\n";
    }
    return out.str();
}
