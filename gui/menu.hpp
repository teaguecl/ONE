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
// What the menu's files share. (The menu sees the emulator only through frontend.h.)
#pragma once

#include <map>
#include <string>
#include <vector>

#include "frontend.h"
#include "imgui.h"

struct ImGuiWindow; // (imgui_internal.h; declared here so Menu::sidebarWindow means ImGui's type in every file)

namespace gui {

// The pages of the menu: the Game page, the settings groups (settings_group(0..n-1)), and the ROM browser
constexpr int kPageGame = -1;
constexpr int kPageBrowser = -2;
constexpr int kPageLua = -3;
constexpr int kPageGameGenie = -4;

// The "press a key or a button" dialog of the settings pages
struct Capture {
    const setting *target = nullptr; // nullptr: not capturing
    bool append = false;             // add the input (else replace the setting's inputs with it)
    bool keysOnly = false;           // SETTING_KEYS: a key with shift/ctrl/alt
    unsigned startedMs = 0;          // SDL_GetTicks() when it started (it gives up after a while)
};

// Choosing a ROM file (or a Lua script)
struct Browser {
    bool scripts = false;             // .lua files (else .nes)
    std::string dir;                  // the directory shown
    std::vector<std::string> entries; // its sub-directories ("name/") and .nes files, sorted; ".." first
    std::string error;                // why the directory could not be read
    bool focusFirst = false;          // put the keyboard/gamepad focus on the first entry (after a change)
};

struct Menu {
    bool gameRunning = false;
    int page = kPageGame;
    bool done = false;
    fe_action result = FE_RESUME;
    bool focusSidebar = true;                     // put the focus on the sidebar's current page (when the menu opens)
    bool focusPage = false;                       // put the focus on the first item of the page (after choosing a page)
    bool sidebarFocused = false;                  // the keyboard/gamepad focus is in the sidebar (else in the page)
    const ::ImGuiWindow *sidebarWindow = nullptr; // (arrow keys between the sidebar and the page)
    const setting *refocus = nullptr; // after the "press a key" dialog: focus this setting's button again ...
    bool refocusAdd = false;          // ... its "+" button (else the one showing the inputs)
    std::string message;              // a line at the bottom: what just happened
    std::string help;                 // the description of the setting under the mouse / the focus
    Capture capture;
    Browser browser;
    std::map<std::string, std::string> textEdits; // SETTING_TEXT / SETTING_FILE being edited, by key
    char ggCode[16] = "";                         // the Game Genie page: the code being typed,
    char ggDescription[96] = "";                  // ... its description,
    char ggFilter[64] = "";                       // ... and the known codes' filter
};

// settings_page.cpp
void drawSettingsPage(Menu &menu, const setting_group &group);
void drawCapture(Menu &menu);                        // the "press a key" dialog (when capturing)
bool captureEvent(Menu &menu, const void *sdlEvent); // an event while capturing: true if it ended the capture

// gamegenie_page.cpp
void drawGameGeniePage(Menu &menu);

// browser.cpp
void openBrowser(Menu &menu, bool scripts = false);
int browserParent(const Menu &menu); // the page the browser goes back to
void drawBrowser(Menu &menu);

// menu.cpp: one line under the item just drawn, when it is hovered or focused: its description
void noteHelp(Menu &menu, const char *text);
// menu.cpp: after a page's first item: give it the keyboard/gamepad focus if the page was just chosen
void focusIfNew(Menu &menu);
void focusLastItem(); // the keyboard/gamepad focus on the item just drawn

} // namespace gui
