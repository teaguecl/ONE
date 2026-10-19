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
#include "window_places.hpp"
#include <SDL.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

// $XDG_STATE_HOME/one/window-places.txt, as frontend.c finds recent.txt; "" if there is no such directory
std::string placesFile()
{
    fs::path dir;
#ifdef _WIN32
    const char *appdata = std::getenv("APPDATA"); // (as the core: %APPDATA%\one\)
    if (appdata != nullptr && *appdata != '\0') {
        dir = fs::path(appdata) / "one";
    } else {
        return {};
    }
#else
    const char *state = std::getenv("XDG_STATE_HOME");
    const char *home = std::getenv("HOME");
    if (state != nullptr && *state != '\0') {
        dir = fs::path(state) / "one";
    } else if (home != nullptr && *home != '\0') {
        dir = fs::path(home) / ".local" / "state" / "one";
    } else {
        return {};
    }
#endif
    return (dir / "window-places.txt").string();
}

// "name x y w h maximized" lines
std::map<std::string, WindowPlace> readPlaces()
{
    std::map<std::string, WindowPlace> places;
    std::ifstream in(placesFile());
    for (std::string line; std::getline(in, line);) {
        std::istringstream words(line);
        std::string name;
        WindowPlace p;
        int maximized = 0;
        if (words >> name >> p.x >> p.y >> p.w >> p.h >> maximized && name[0] != '#' && p.w > 0 && p.h > 0) {
            p.known = true;
            p.maximized = maximized != 0;
            places[name] = p;
        }
    }
    return places;
}

// At least a corner of the window (100 x 50 pixels of its top) is on one of the screens
bool onScreen(const WindowPlace &p)
{
    for (int i = 0; i < SDL_GetNumVideoDisplays(); i++) {
        SDL_Rect screen;
        if (SDL_GetDisplayBounds(i, &screen) != 0) {
            continue;
        }
        SDL_Rect top{p.x, p.y, p.w, 50}, overlap;
        if (SDL_IntersectRect(&top, &screen, &overlap) && overlap.w >= std::min(100, p.w)) {
            return true;
        }
    }
    return false;
}

} // namespace

WindowPlace savedWindowPlace(const std::string &name)
{
    std::map<std::string, WindowPlace> places = readPlaces();
    auto it = places.find(name);
    if (it == places.end() || !onScreen(it->second)) {
        return {};
    }
    return it->second;
}

void saveWindowPlace(const std::string &name, SDL_Window *window)
{
    const std::string file = placesFile();
    if (window == nullptr || file.empty() || (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0) {
        return;
    }
    WindowPlace p;
    p.known = true;
    p.maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
    SDL_GetWindowPosition(window, &p.x, &p.y);
    SDL_GetWindowSize(window, &p.w, &p.h);
    std::map<std::string, WindowPlace> places = readPlaces();
    places[name] = p;
    std::error_code ec;
    fs::create_directories(fs::path(file).parent_path(), ec);
    std::ofstream out(file);
    out << "# one-debugger: where its windows were when it last quit (name x y width height maximized)\n";
    for (const auto &[key, place] : places) {
        out << key << ' ' << place.x << ' ' << place.y << ' ' << place.w << ' ' << place.h << ' '
            << (place.maximized ? 1 : 0) << '\n';
    }
}

void moveWindowTo(SDL_Window *window, const WindowPlace &place)
{
    if (window == nullptr || !place.known) {
        return;
    }
    SDL_SetWindowSize(window, place.w, place.h);
    SDL_SetWindowPosition(window, place.x, place.y);
    if (place.maximized) {
        SDL_MaximizeWindow(window);
    }
}
