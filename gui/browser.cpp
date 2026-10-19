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
// Choosing a ROM (or a Lua script): a list of the directory's sub-directories and .nes (.lua) files, usable with a
// gamepad too
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "frontend.h"
#include "menu.hpp"

namespace fs = std::filesystem;

namespace gui {

namespace {

std::string lastScriptDir; // where the last script was chosen (for as long as ONE runs)

bool wanted(const Browser &browser, const fs::path &path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return browser.scripts ? ext == ".lua" : (ext == ".nes" || ext == ".zip");
}

// The top of the tree: on Windows the list of drives (an empty directory name); elsewhere "/" is the top
bool atTop(const fs::path &dir)
{
#ifdef _WIN32
    return dir.has_root_path() && dir.parent_path() == dir;
#else
    (void)dir;
    return false;
#endif
}

void readDrives(Browser &browser)
{
#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (mask & (1u << i)) {
            browser.entries.push_back(std::string(1, static_cast<char>('A' + i)) + ":/");
        }
    }
#else
    (void)browser;
#endif
}

void readDir(Browser &browser)
{
    browser.entries.clear();
    browser.error.clear();
    browser.focusFirst = true;
    std::error_code ec;
    fs::path dir(browser.dir);
    if (browser.dir.empty()) { // (Windows: the drives)
        readDrives(browser);
        return;
    }
    if ((dir.has_parent_path() && dir.parent_path() != dir) || atTop(dir)) {
        browser.entries.emplace_back("../");
    }
    std::vector<std::string> dirs;
    std::vector<std::string> files;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') {
            continue;
        }
        std::error_code type_ec;
        if (it->is_directory(type_ec)) {
            dirs.push_back(name + "/");
        } else if (wanted(browser, it->path())) {
            files.push_back(name);
        }
    }
    if (ec) {
        browser.error = ec.message();
    }
    auto byName = [](const std::string &a, const std::string &b) {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(),
            [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    browser.entries.insert(browser.entries.end(), dirs.begin(), dirs.end());
    browser.entries.insert(browser.entries.end(), files.begin(), files.end());
}

void changeDir(Browser &browser, const fs::path &dir)
{
    if (dir.empty() && !browser.dir.empty()) { // (up from the top: the drives)
        browser.dir.clear();
        readDir(browser);
        return;
    }
    std::error_code ec;
    fs::path absolute = fs::absolute(dir, ec);
    browser.dir = (ec ? dir : absolute.lexically_normal()).string();
    bool driveRoot = browser.dir.size() == 3 && browser.dir[1] == ':'; // ("C:/" is not "C:")
    if (!driveRoot && browser.dir.size() > 1 && (browser.dir.back() == '/' || browser.dir.back() == '\\')) {
        browser.dir.pop_back();
    }
    readDir(browser);
}

} // namespace

int browserParent(const Menu &menu)
{
    return menu.browser.scripts ? kPageLua : kPageGame;
}

void openBrowser(Menu &menu, bool scripts)
{
    menu.page = kPageBrowser;
    menu.message.clear();
    std::string rom = fe_rom_path();
    if (menu.browser.dir.empty() || menu.browser.scripts != scripts) {
        // games: the ROM's directory, else the current one; scripts: where the last one was chosen, else the
        // newest recent script's directory, else as for games
        menu.browser.scripts = scripts;
        std::error_code ec;
        fs::path start = rom.empty() ? fs::current_path(ec) : fs::path(rom).parent_path();
        if (scripts && !lastScriptDir.empty()) {
            start = lastScriptDir;
        } else if (scripts && fe_recent_count(FE_RECENT_SCRIPTS) > 0) {
            start = fs::path(fe_recent(FE_RECENT_SCRIPTS, 0)).parent_path();
        }
        changeDir(menu.browser, start.empty() ? fs::path(".") : start);
    } else {
        readDir(menu.browser);
    }
}

void drawBrowser(Menu &menu)
{
    Browser &browser = menu.browser;
    ImGui::TextWrapped("%s", browser.dir.empty() ? "Drives" : browser.dir.c_str());
    if (!browser.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", browser.error.c_str());
    }
    ImGui::Separator();
    if (browser.entries.empty() || (browser.entries.size() == 1 && browser.entries[0] == "../")) {
        ImGui::TextDisabled(browser.scripts ? "(no .lua files here)" : "(no .nes or .zip files here)");
    }
    for (size_t i = 0; i < browser.entries.size(); i++) {
        const std::string &name = browser.entries[i];
        bool clicked = ImGui::Selectable(name.c_str());
        if (browser.focusFirst && (i == 1 || browser.entries.size() == 1)) { // the first entry after ".."
            menu.focusPage = true;
            focusIfNew(menu);
            browser.focusFirst = false;
        }
        if (clicked) {
            fs::path path = fs::path(browser.dir) / name;
            if (name == "../") {
                fs::path here(browser.dir);
                changeDir(browser, atTop(here) ? fs::path() : here.parent_path());
            } else if (browser.dir.empty()) { // (a drive: "Z:/")
                changeDir(browser, fs::path(name));
            } else if (name.back() == '/') {
                changeDir(browser, fs::path(browser.dir) / name.substr(0, name.size() - 1));
            } else if (browser.scripts) {
                lastScriptDir = browser.dir;
                menu.page = kPageLua;
                menu.focusPage = true;
                menu.message = fe_script_load(path.string().c_str()) ? "Started " + name
                                                                     : std::string("Error: ") + fe_script_error();
            } else {
                fe_set_next_rom(path.string().c_str());
                menu.result = FE_OPEN_ROM;
                menu.done = true;
            }
            break; // (the list just changed)
        }
        noteHelp(menu,
                 name.back() == '/' ? "Open this directory" : (browser.scripts ? "Run this script" : "Play this game"));
    }
    browser.focusFirst = false;
}

} // namespace gui
