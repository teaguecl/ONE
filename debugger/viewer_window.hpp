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
// What the debugger's picture windows (wbg: bg_viewer.hpp, wpal: palette_viewer.hpp) have in common: a window of
// their own beside the game window, showing a picture (pixels, 0x00RRGGBB) at a whole multiple of its size, sharp;
// resizable; reopened where it was last time (window_places); redrawn at every stop and, while the game runs, once a
// frame (refreshAll()); its events taken from the others' (handleEventAll()); savable as a PNG (also in --batch).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
union SDL_Event;

class ViewerWindow {
public:
    virtual ~ViewerWindow() = default;
    ViewerWindow(const ViewerWindow &) = delete;
    ViewerWindow &operator=(const ViewerWindow &) = delete;

    bool isOpen() const { return window != nullptr; }
    bool open(std::string &error); // (where it was last time: window_places)
    void close();                  // (remembers where it was)
    void refresh();                // draw it again from the emulator's state (nothing if it is closed)
    // An SDL event for its window: handled (the close button closes it; keys and the mouse go to onKey / onMouse).
    // false: the event is another window's.
    bool handleEvent(const SDL_Event &event);
    bool save(const std::string &file); // the picture as a PNG (drawn now; also without the window)

    // every viewer there is (the singletons made so far)
    static bool handleEventAll(const SDL_Event &event);
    static void refreshAll();
    static void closeAll();

protected:
    // PLACE: its name in window_places; WIDTH x HEIGHT: the picture, shown SCALE times
    ViewerWindow(const char *place, int width, int height, int scale);
    virtual void render() = 0;                    // pixels from the emulator's state
    virtual std::string title() const = 0;        // (updateTitle() puts it on the window)
    virtual void onKey(int /*sdlKey*/) {}         // a key pressed in the window (SDL_Keycode, no modifiers)
    virtual void onKeyRepeat(int /*sdlKey*/) {}   // the same key again, while it is held (auto-repeat)
    virtual void onOpened() {}                    // the window has just opened
    virtual void onClosed() {}                    // it has just closed (also at exit)
    virtual void onMouse(int /*x*/, int /*y*/) {} // the mouse over the picture (pixels), or -1, -1: not over it
    void updateTitle();
    void present();                                        // show the pixels as they are
    void fill(int x, int y, int w, int h, uint32_t color); // a rectangle of the picture (clipped)
    // TEXT in the 5x7 font of font5x7.h, a character every 6 pixels (the example Lua scripts draw it the same way)
    void text(int x, int y, const std::string &s, uint32_t color);
    std::vector<uint32_t> pixels;
    const int width, height;

private:
    const char *place;
    int scale;
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
};
