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
#include "pattern_viewer.hpp"
#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "core.hpp"
#include "debugger/viewer_window.hpp"

// The layout (lua/examples/pattern.lua has the same numbers: the two pictures are compared in a test)
namespace {
constexpr uint32_t kBackground = 0x202020, kLabel = 0xC0C0C0, kSeparator = 0x606060;
constexpr int kTableLeft[2] = {4, 141}, kTableTop = 14, kSeparatorX = 136, kLabelTop = 4, kInfoTop = 146;
const char *const kPaletteNames[9] = {"BG0", "BG1", "BG2", "BG3", "SP0", "SP1", "SP2", "SP3", "GRAY"};
} // namespace

PatternViewer &PatternViewer::instance()
{
    static PatternViewer viewer;
    return viewer;
}

PatternViewer::PatternViewer() : ViewerWindow("pattern", kWidth, kHeight, 2) {}

std::string PatternViewer::title() const
{
    std::string hover = hoverText();
    return std::string("ONE Debugger - Pattern tables - ") + kPaletteNames[palette] + " (P: palette, G: grid)" +
           (hover.empty() ? std::string() : " - " + hover);
}

void PatternViewer::onKey(int key)
{
    if (key == SDLK_p) {
        palette = (palette + 1) % 9;
    } else if (key == SDLK_g) {
        grid = !grid;
    } else {
        return;
    }
    updateTitle();
    refresh();
}

void PatternViewer::onMouse(int x, int y)
{
    if (x != mouseX || y != mouseY) {
        mouseX = x;
        mouseY = y;
        updateTitle();
        refresh();
    }
}

// The tile under the mouse: its number, its PPU address and where it comes from in CHR-ROM / CHR-RAM ("TILE $1A  $11A0
// CHR $005A0"); "" when the mouse is over neither table
std::string PatternViewer::hoverText() const
{
    for (int t = 0; t < 2; t++) {
        const int x = mouseX - kTableLeft[t], y = mouseY - kTableTop;
        if (x >= 0 && x < 128 && y >= 0 && y < 128) {
            const int tile = (y / 8) * 16 + x / 8;
            const uint16_t address = static_cast<uint16_t>(t * 0x1000 + tile * 16);
            char buffer[64];
            int n = std::snprintf(buffer, sizeof(buffer), "TILE $%02X  $%04X", tile, address);
            const int32_t offset = mapper_debug_chr_offset(address);
            if (offset >= 0) {
                std::snprintf(buffer + n, sizeof(buffer) - static_cast<size_t>(n), "  CHR $%05X", offset);
            }
            return buffer;
        }
    }
    return {};
}

void PatternViewer::render()
{
    pixels.assign(static_cast<size_t>(kWidth) * kHeight, kBackground);
    // the four colors: palette RAM's (color 0: the backdrop, as the PPU draws it), or grays
    uint32_t colors[4] = {0x000000, 0x555555, 0xAAAAAA, 0xFFFFFF};
    if (palette < 8) {
        for (int i = 0; i < 4; i++) {
            uint8_t entry = ppu_debug_read_palette(static_cast<uint8_t>(i == 0 ? 0 : palette * 4 + i));
            colors[i] = video_palette_rgb(entry & 0x3F) & 0xFFFFFF;
        }
    }
    const uint8_t ctrl = ppu_get_ppuctrl();
    for (int t = 0; t < 2; t++) {
        // the label: which table this is, and what PPUCTRL uses it for (8x16 sprites use both)
        std::string label = t == 0 ? "$0000" : "$1000";
        if (((ctrl >> 4) & 1) == t) {
            label += " BG";
        }
        if ((ctrl & 0x20) != 0 || ((ctrl >> 3) & 1) == t) {
            label += " SP";
        }
        text(kTableLeft[t], kLabelTop, label, kLabel);
        for (int tile = 0; tile < 256; tile++) {
            const int left = kTableLeft[t] + (tile % 16) * 8, top = kTableTop + (tile / 16) * 8;
            for (int row = 0; row < 8; row++) {
                const uint16_t address = static_cast<uint16_t>(t * 0x1000 + tile * 16 + row);
                const uint8_t low = ppu_debug_read(address), high = ppu_debug_read(static_cast<uint16_t>(address + 8));
                for (int x = 0; x < 8; x++) {
                    const int bit = 7 - x;
                    uint32_t color = colors[((low >> bit) & 1) | (((high >> bit) & 1) << 1)];
                    if (grid && (x == 0 || row == 0)) {
                        color = ((color >> 1) & 0x7F7F7F) + 0x404040; // (the grid: lightened)
                    }
                    pixels[static_cast<size_t>(top + row) * kWidth + left + x] = color;
                }
            }
        }
    }
    fill(kSeparatorX, kLabelTop, 1, kTableTop + 128 - kLabelTop, kSeparator);
    text(4, kInfoTop, hoverText(), kLabel);
}
