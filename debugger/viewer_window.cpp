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
#include "viewer_window.hpp"
#include <SDL.h>

#include <algorithm>
#include <vector>
#include <cstddef>
#include <string>
#include <cstdint>

#include "window_places.hpp"
extern "C" {
#include "font5x7.h"
#include "png.h"
}

namespace {
std::vector<ViewerWindow *> &viewers()
{
    static std::vector<ViewerWindow *> list;
    return list;
}
} // namespace

ViewerWindow::ViewerWindow(const char *place, int width, int height, int scale)
    : pixels(static_cast<size_t>(width) * height, 0), width(width), height(height), place(place), scale(scale)
{
    viewers().push_back(this);
}

bool ViewerWindow::handleEventAll(const SDL_Event &event)
{
    for (ViewerWindow *viewer : viewers()) {
        if (viewer->handleEvent(event)) {
            return true;
        }
    }
    return false;
}

void ViewerWindow::refreshAll()
{
    for (ViewerWindow *viewer : viewers()) {
        viewer->refresh();
    }
}

void ViewerWindow::closeAll()
{
    for (ViewerWindow *viewer : viewers()) {
        viewer->close();
    }
}

bool ViewerWindow::open(std::string &error)
{
    if (window != nullptr) {
        return true;
    }
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) { // (-H: no game window, no video yet)
        error = std::string("cannot start SDL video: ") + SDL_GetError();
        return false;
    }
    window = SDL_CreateWindow(title().c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width * scale,
                              height * scale, SDL_WINDOW_RESIZABLE);
    if (window != nullptr) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (renderer == nullptr) {
            renderer = SDL_CreateRenderer(window, -1, 0);
        }
    }
    if (renderer != nullptr) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest"); // (sharp pixels when the window is made larger)
        SDL_RenderSetLogicalSize(renderer, width, height);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
    }
    if (texture == nullptr) {
        error = std::string("cannot open a window: ") + SDL_GetError();
        close();
        return false;
    }
    moveWindowTo(window, savedWindowPlace(place));
    onOpened();
    refresh();
    return true;
}

void ViewerWindow::close()
{
    const bool wasOpen = window != nullptr;
    if (window != nullptr && texture != nullptr) {
        saveWindowPlace(place, window);
    }
    if (texture != nullptr) {
        SDL_DestroyTexture(texture);
        texture = nullptr;
    }
    if (renderer != nullptr) {
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }
    if (window != nullptr) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    if (wasOpen) {
        onClosed();
    }
}

void ViewerWindow::updateTitle()
{
    if (window != nullptr) {
        SDL_SetWindowTitle(window, title().c_str());
    }
}

bool ViewerWindow::handleEvent(const SDL_Event &event)
{
    if (window == nullptr) {
        return false;
    }
    Uint32 id = 0;
    switch (event.type) {
    case SDL_WINDOWEVENT: id = event.window.windowID; break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: id = event.key.windowID; break;
    case SDL_TEXTEDITING: id = event.edit.windowID; break;
    case SDL_TEXTINPUT: id = event.text.windowID; break;
    case SDL_MOUSEMOTION: id = event.motion.windowID; break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: id = event.button.windowID; break;
    case SDL_MOUSEWHEEL: id = event.wheel.windowID; break;
    default: return false;
    }
    if (id != SDL_GetWindowID(window)) {
        return false;
    }
    if (event.type == SDL_KEYDOWN && (event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) == 0) {
        if (event.key.repeat == 0) {
            onKey(event.key.keysym.sym);
        } else {
            onKeyRepeat(event.key.keysym.sym);
        }
    } else if (event.type == SDL_MOUSEMOTION) {
        // (SDL has made the position the picture's already: the renderer has a logical size)
        const int x = event.motion.x, y = event.motion.y;
        const bool inside = x >= 0 && y >= 0 && x < width && y < height;
        onMouse(inside ? x : -1, inside ? y : -1);
    } else if (event.type == SDL_WINDOWEVENT) {
        if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
            close();
        } else if (event.window.event == SDL_WINDOWEVENT_LEAVE) {
            onMouse(-1, -1);
        } else if (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                   event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            present();
        }
    }
    return true; // (its keys are not the game's)
}

void ViewerWindow::refresh()
{
    if (window == nullptr) {
        return;
    }
    render();
    present();
}

void ViewerWindow::present()
{
    if (texture == nullptr) {
        return;
    }
    SDL_UpdateTexture(texture, nullptr, pixels.data(), width * static_cast<int>(sizeof(uint32_t)));
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
}

bool ViewerWindow::save(const std::string &file)
{
    render();
    std::vector<uint8_t> rgb(pixels.size() * 3);
    for (size_t i = 0; i < pixels.size(); i++) {
        rgb[i * 3] = static_cast<uint8_t>(pixels[i] >> 16);
        rgb[i * 3 + 1] = static_cast<uint8_t>(pixels[i] >> 8);
        rgb[i * 3 + 2] = static_cast<uint8_t>(pixels[i]);
    }
    present(); // (if it is open: what was just rendered)
    return png_write_rgb(file.c_str(), rgb.data(), width, height);
}

void ViewerWindow::fill(int x, int y, int w, int h, uint32_t color)
{
    for (int row = std::max(0, y); row < std::min(height, y + h); row++) {
        for (int col = std::max(0, x); col < std::min(width, x + w); col++) {
            pixels[static_cast<size_t>(row) * width + col] = color;
        }
    }
}

void ViewerWindow::text(int x, int y, const std::string &s, uint32_t color)
{
    for (char c : s) {
        if (c >= 32 && c <= 126) {
            const uint8_t *glyph = font5x7[c - 32];
            for (int row = 0; row < 7; row++) {
                for (int col = 0; col < 5; col++) {
                    if (((glyph[row] >> (4 - col)) & 1) != 0) {
                        fill(x + col, y + row, 1, 1, color);
                    }
                }
            }
        }
        x += 6;
    }
}
