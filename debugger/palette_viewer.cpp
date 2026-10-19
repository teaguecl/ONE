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
#include "palette_viewer.hpp"
#include <cstdint>
#include <cstdio>
#include <string>

#include "core.hpp"
#include "debugger/viewer_window.hpp"

// The layout (lua/examples/palette.lua has the same numbers: the two pictures are compared in a test)
namespace {
constexpr uint32_t kBackground = 0x202020, kLabel = 0xC0C0C0;
constexpr int kRamTop = 4, kRamRowPitch = 18, kSwatchLeft = 26, kSwatchPitch = 26, kSwatchW = 24, kSwatchH = 16;
constexpr int kGridLeft = 4, kGridTop = 152, kCellPitchX = 17, kCellPitchY = 13, kCellW = 16, kCellH = 12;
constexpr int kInfoTop = 207;

uint32_t rgbOf(uint8_t value)
{
    return video_palette_rgb(value & 0x3F) & 0xFFFFFF;
}

// black or white, whichever reads better on COLOR
uint32_t contrast(uint32_t color)
{
    const uint32_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    return (r * 299 + g * 587 + b * 114) / 1000 >= 128 ? 0x000000 : 0xFFFFFF;
}

std::string hex2(unsigned value)
{
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02X", value);
    return buffer;
}

} // namespace

PaletteViewer &PaletteViewer::instance()
{
    static PaletteViewer viewer;
    return viewer;
}

PaletteViewer::PaletteViewer() : ViewerWindow("palette", kWidth, kHeight, 1) {}

std::string PaletteViewer::title() const
{
    std::string hover = hoverText();
    return "ONE Debugger - Palette" + (hover.empty() ? std::string() : " - " + hover);
}

void PaletteViewer::onMouse(int x, int y)
{
    if (x != mouseX || y != mouseY) {
        mouseX = x;
        mouseY = y;
        updateTitle();
        refresh();
    }
}

// What the mouse is over: a palette RAM entry ("$3F05 BG1:1 = $27 #F0BC3C") or one of the 64 colors
// ("COLOR $27 #F0BC3C"); "" when nothing
std::string PaletteViewer::hoverText() const
{
    char buffer[64];
    for (int entry = 0; entry < 32; entry++) {
        const int x = kSwatchLeft + (entry & 3) * kSwatchPitch, y = kRamTop + (entry >> 2) * kRamRowPitch;
        if (mouseX >= x && mouseX < x + kSwatchW && mouseY >= y && mouseY < y + kSwatchH) {
            const uint8_t value = ppu_debug_read_palette(static_cast<uint8_t>(entry)) & 0x3F;
            std::snprintf(buffer, sizeof(buffer), "$3F%02X %s%d:%d = $%02X #%06X", entry, entry < 16 ? "BG" : "SP",
                          (entry >> 2) & 3, entry & 3, value, rgbOf(value));
            return buffer;
        }
    }
    for (int color = 0; color < 64; color++) {
        const int x = kGridLeft + (color & 15) * kCellPitchX, y = kGridTop + (color >> 4) * kCellPitchY;
        if (mouseX >= x && mouseX < x + kCellW && mouseY >= y && mouseY < y + kCellH) {
            std::snprintf(buffer, sizeof(buffer), "COLOR $%02X #%06X", color, rgbOf(static_cast<uint8_t>(color)));
            return buffer;
        }
    }
    return {};
}

void PaletteViewer::render()
{
    pixels.assign(static_cast<size_t>(kWidth) * kHeight, kBackground);
    bool used[64] = {};
    // palette RAM: a row per palette
    for (int entry = 0; entry < 32; entry++) {
        const int row = entry >> 2, i = entry & 3;
        const int x = kSwatchLeft + i * kSwatchPitch, y = kRamTop + row * kRamRowPitch;
        if (i == 0) {
            text(4, y + 4, std::string(row < 4 ? "BG" : "SP") + std::to_string(row & 3), kLabel);
        }
        const uint8_t value = ppu_debug_read_palette(static_cast<uint8_t>(entry)) & 0x3F;
        used[value] = true;
        const uint32_t color = rgbOf(value), ink = contrast(color);
        fill(x, y, kSwatchW, kSwatchH, color);
        text(x + 3, y + 4, "$" + hex2(value), ink);
        if (entry >= 16 && i == 0) { // ($3F10 $3F14 $3F18 $3F1C: the same bytes as $3F00 $3F04 $3F08 $3F0C)
            fill(x + 1, y + 1, 3, 1, ink);
            fill(x + 1, y + 1, 1, 3, ink);
        }
    }
    // the 64 colors, 16 a row; the ones palette RAM uses outlined
    for (int value = 0; value < 64; value++) {
        const int x = kGridLeft + (value & 15) * kCellPitchX, y = kGridTop + (value >> 4) * kCellPitchY;
        const uint32_t color = rgbOf(static_cast<uint8_t>(value)), ink = contrast(color);
        fill(x, y, kCellW, kCellH, color);
        text(x + 2, y + 2, hex2(static_cast<unsigned>(value)), ink);
        if (used[value]) {
            fill(x, y, kCellW, 1, ink);
            fill(x, y + kCellH - 1, kCellW, 1, ink);
            fill(x, y, 1, kCellH, ink);
            fill(x + kCellW - 1, y, 1, kCellH, ink);
        }
    }
    text(4, kInfoTop, hoverText(), kLabel);
}
