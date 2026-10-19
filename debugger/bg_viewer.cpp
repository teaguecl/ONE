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
#include "bg_viewer.hpp"
#include <SDL.h>

#include <algorithm>
#include <string>
#include <cstdint>
#include <cstddef>
#include <utility>
#include <vector>

#include "core.hpp"
#include "debugger/viewer_window.hpp"

BackgroundViewer &BackgroundViewer::instance()
{
    static BackgroundViewer viewer;
    return viewer;
}

BackgroundViewer::BackgroundViewer() : ViewerWindow("background", kWidth, kHeight, 1) {}

// The title says how the part shown is drawn, and the key that changes it
std::string BackgroundViewer::title() const
{
    return perLine ? "ONE Debugger - Background - overlay: each line (O: one rectangle)"
                   : "ONE Debugger - Background - overlay: one rectangle (O: each line)";
}

void BackgroundViewer::onKey(int key)
{
    if (key == SDLK_o) { // O: the overlay line by line, or one rectangle
        perLine = !perLine;
        updateTitle();
        refresh();
    }
}

namespace {

// Where a picture line's background pattern bytes came from: the pattern table (PPUCTRL bit 4) and its four 1KB pages
// as CHR offsets (ppu_line_scroll::bg_chr); -1: the mapper does not say (read through the current banks instead)
struct Patterns {
    uint16_t table = 0;
    int32_t page[4] = {-1, -1, -1, -1};
    bool operator==(const Patterns &o) const
    {
        return table == o.table && page[0] == o.page[0] && page[1] == o.page[1] && page[2] == o.page[2] &&
               page[3] == o.page[3];
    }
};

Patterns patternsOf(const ppu_line_scroll &line)
{
    Patterns p;
    p.table = (line.ctrl & 0x10) ? 0x1000 : 0x0000;
    for (int i = 0; i < 4; i++) {
        p.page[i] = line.bg_chr[i];
    }
    return p;
}

Patterns currentPatterns()
{
    Patterns p;
    p.table = (ppu_get_ppuctrl() & 0x10) ? 0x1000 : 0x0000;
    for (int i = 0; i < 4; i++) {
        p.page[i] = mapper_debug_chr_offset(static_cast<uint16_t>(p.table | (i << 10)));
    }
    return p;
}

// Draws rows of the 512 x 480 map: the nametables through the mirroring, a row's pattern bytes from PATTERNS
class MapPainter {
public:
    explicit MapPainter(std::vector<uint32_t> &pixels) : pixels(pixels)
    {
        chr = mapper_debug_chr_data(&chrSize);
        for (int i = 0; i < 32; i++) {
            uint8_t entry = ppu_debug_read_palette(static_cast<uint8_t>((i & 3) == 0 ? 0 : i)); // (0: the backdrop)
            colors[i] = video_palette_rgb(entry & 0x3F) & 0xFFFFFF;
        }
    }

    // COUNT pixels of map row Y from column X0 (wrapping at the right edge)
    void row(int y, int x0, int count, const Patterns &patterns)
    {
        const int tableRow = y / 240, ty = (y % 240) / 8, fineY = y % 8;
        int lastColumn = -1, palette = 0;
        uint8_t low = 0, high = 0;
        for (int i = 0; i < count; i++) {
            const int x = (x0 + i) % BackgroundViewer::kWidth, column = x / 8;
            if (column != lastColumn) {
                lastColumn = column;
                const int tx = column % 32;
                const uint16_t base = static_cast<uint16_t>(0x2000 + (tableRow * 2 + column / 32) * 0x400);
                const uint8_t tile = ppu_debug_read(static_cast<uint16_t>(base + ty * 32 + tx));
                const uint8_t attribute = ppu_debug_read(static_cast<uint16_t>(base + 0x3C0 + (ty / 4) * 8 + tx / 4));
                palette = (attribute >> (((ty & 2) << 1) | (tx & 2))) & 3;
                int32_t ownBank = 0; // (MMC5 extended attributes: this tile has its own CHR bank and palette)
                int ownPalette = 0;
                if (chr != nullptr &&
                    mapper_debug_bg_tile(static_cast<uint16_t>(base + ty * 32 + tx), &ownBank, &ownPalette) &&
                    static_cast<size_t>(ownBank) + 0x1000 <= chrSize) {
                    palette = ownPalette;
                    low = chr[static_cast<size_t>(ownBank) + ((tile * 16 + fineY) & 0xFFF)];
                    high = chr[static_cast<size_t>(ownBank) + ((tile * 16 + fineY + 8) & 0xFFF)];
                } else {
                    low = pattern(patterns, tile * 16 + fineY);
                    high = pattern(patterns, tile * 16 + fineY + 8);
                }
            }
            const int bit = 7 - (x % 8);
            const int color = ((low >> bit) & 1) | (((high >> bit) & 1) << 1);
            pixels[static_cast<size_t>(y) * BackgroundViewer::kWidth + x] = colors[palette * 4 + color];
        }
    }

private:
    // a byte of the pattern table (ADDRESS: 0-$FFF in it) as PATTERNS maps it
    uint8_t pattern(const Patterns &patterns, int address) const
    {
        const int32_t page = patterns.page[(address >> 10) & 3];
        if (chr != nullptr && page >= 0 && static_cast<size_t>(page) + (address & 0x3FF) < chrSize) {
            return chr[static_cast<size_t>(page) + (address & 0x3FF)];
        }
        return ppu_debug_read(static_cast<uint16_t>(patterns.table + address)); // (through the banks of now)
    }
    std::vector<uint32_t> &pixels;
    const uint8_t *chr = nullptr;
    size_t chrSize = 0;
    uint32_t colors[32] = {};
};

} // namespace

// The four nametables with the palettes as they are now. Games switch the background's CHR banks (or pattern table)
// in the middle of the picture -- a status bar's letters, a cinema scene's picture -- so each line the game window
// shows is drawn with the patterns it was drawn with (ppu_debug_line_scroll()), and the rest of the map with the ones
// most of the picture used. Then the part the game window shows, line by line from the scroll each line started
// with: full brightness, a white outline on its edge; everything else at half brightness. A line without background
// (PPUMASK bit 3 clear) shows nothing; nor do the leftmost 8 pixels of a line whose background is clipped there
// (PPUMASK bit 1 clear).
void BackgroundViewer::render()
{
    pixels.assign(static_cast<size_t>(kWidth) * kHeight, 0);
    const ppu_line_scroll *lines = ppu_debug_line_scroll();

    // the patterns most of the picture's lines used (none: the ones mapped now)
    std::vector<std::pair<Patterns, int>> counts;
    for (int line = 0; line < 240; line++) {
        if ((lines[line].mask & 0x08) == 0) {
            continue;
        }
        Patterns p = patternsOf(lines[line]);
        auto it = std::find_if(counts.begin(), counts.end(), [&](const auto &c) { return c.first == p; });
        if (it == counts.end()) {
            counts.emplace_back(p, 1);
        } else {
            it->second++;
        }
    }
    Patterns common = currentPatterns();
    int most = 0;
    for (const auto &[p, n] : counts) {
        if (n > most) {
            most = n;
            common = p;
        }
    }
    MapPainter painter(pixels);
    for (int y = 0; y < kHeight; y++) {
        painter.row(y, 0, kWidth, common);
    }
    for (int line = 0; line < 240; line++) { // (the lines drawn with other patterns, where they show)
        const ppu_line_scroll &s = lines[line];
        const int coarseY = (s.v >> 5) & 0x1F;
        if ((s.mask & 0x08) == 0 || coarseY >= 30 || patternsOf(s) == common) {
            continue;
        }
        const int x0 = ((s.v >> 10) & 1) * 256 + (s.v & 0x1F) * 8 + s.fine_x;
        painter.row(((s.v >> 11) & 1) * 240 + coarseY * 8 + ((s.v >> 12) & 7), x0, 256, patternsOf(s));
    }

    // what the game window shows: each line's 256 pixels, from where its scroll put them in the 512 x 480 map (O in
    // the window: one 256 x 240 rectangle instead, from the scroll the picture starts with, as Mesen draws it)
    std::vector<uint8_t> shown(static_cast<size_t>(kWidth) * kHeight, 0);
    if (!perLine) {
        for (int line = 0; line < 240; line++) {
            const ppu_line_scroll &s = lines[line];
            const int coarseY = (s.v >> 5) & 0x1F;
            if ((s.mask & 0x08) == 0 || coarseY >= 30) {
                continue; // (the first line with background in the picture starts the rectangle)
            }
            const int x0 = ((s.v >> 10) & 1) * 256 + (s.v & 0x1F) * 8 + s.fine_x;
            const int y0 = ((s.v >> 11) & 1) * 240 + coarseY * 8 + ((s.v >> 12) & 7) - line;
            for (int y = 0; y < 240; y++) {
                for (int i = 0; i < 256; i++) {
                    shown[static_cast<size_t>((y0 + y + kHeight) % kHeight) * kWidth +
                          static_cast<size_t>((x0 + i) % kWidth)] = 1;
                }
            }
            break;
        }
    }
    for (int line = 0; line < 240 && perLine; line++) {
        const ppu_line_scroll &s = lines[line];
        if ((s.mask & 0x08) == 0) {
            continue; // (no background on this line)
        }
        const int coarseX = s.v & 0x1F, coarseY = (s.v >> 5) & 0x1F, fineY = (s.v >> 12) & 7;
        if (coarseY >= 30) {
            continue; // (the line comes from the attribute table's rows: not part of the picture of the map)
        }
        const int x0 = ((s.v >> 10) & 1) * 256 + coarseX * 8 + s.fine_x;
        const int y = ((s.v >> 11) & 1) * 240 + coarseY * 8 + fineY;
        for (int i = (s.mask & 0x02) ? 0 : 8; i < 256; i++) {
            shown[static_cast<size_t>(y) * kWidth + static_cast<size_t>((x0 + i) % kWidth)] = 1;
        }
    }
    auto isShown = [&](int x, int y) { // (the map wraps around: so does the part shown)
        return shown[static_cast<size_t>((y + kHeight) % kHeight) * kWidth +
                     static_cast<size_t>((x + kWidth) % kWidth)] != 0;
    };
    for (int y = 0; y < kHeight; y++) {
        for (int x = 0; x < kWidth; x++) {
            uint32_t &p = pixels[static_cast<size_t>(y) * kWidth + x];
            if (!isShown(x, y)) {
                p = (p >> 1) & 0x7F7F7F; // half brightness
            } else if (!isShown(x - 1, y) || !isShown(x + 1, y) || !isShown(x, y - 1) || !isShown(x, y + 1)) {
                p = 0xFFFFFF; // the edge of the part shown
            }
        }
    }
}
