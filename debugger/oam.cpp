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
#include "oam.hpp"
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "core.hpp"

namespace {

#if defined(__GNUC__)
#ifdef __MINGW32__
#define ONE_PRINTF_ARCHETYPE gnu_printf // (the C library is MinGW's own with __USE_MINGW_ANSI_STDIO: %zu, %llu)
#else
#define ONE_PRINTF_ARCHETYPE printf
#endif
std::string format(const char *fmt, ...) __attribute__((format(ONE_PRINTF_ARCHETYPE, 1, 2)));
#endif
std::string format(const char *fmt, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

} // namespace

OamReport decodeOam()
{
    OamReport r;
    const uint8_t ctrl = ppu_get_ppuctrl();
    const uint8_t mask = ppu_get_ppumask();
    r.tall = (ctrl & 0x20) != 0;
    r.table = (ctrl & 0x08) ? 0x1000 : 0x0000;
    r.spritesOn = (mask & 0x10) != 0;
    r.leftShown = (mask & 0x04) != 0;
    const int height = r.tall ? 16 : 8;
    for (int i = 0; i < 64; i++) {
        OamSprite &s = r.sprites[static_cast<size_t>(i)];
        s.index = i;
        s.y = ppu_debug_read_oam(static_cast<uint8_t>(i * 4));
        s.tile = ppu_debug_read_oam(static_cast<uint8_t>(i * 4 + 1));
        s.attr = ppu_debug_read_oam(static_cast<uint8_t>(i * 4 + 2));
        s.x = ppu_debug_read_oam(static_cast<uint8_t>(i * 4 + 3));
        s.top = s.y + 1; // (OAM holds the line before the sprite's first one)
        s.bottom = s.top + height - 1;
        s.hidden = s.top > 239;
        s.leftMasked = !s.hidden && s.x < 8 && !r.leftShown;
        r.visible += s.hidden ? 0 : 1;
    }
    // Per picture line, in OAM order: the first 8 sprites are drawn, the rest dropped
    bool inOverflow = false;
    for (int line = 1; line < 240; line++) {
        int count = 0;
        for (OamSprite &s : r.sprites) {
            if (s.hidden || line < s.top || line > s.bottom) {
                continue;
            }
            if (++count > 8) {
                if (s.droppedFirst < 0) {
                    s.droppedFirst = line;
                }
                s.droppedLast = line;
            }
        }
        if (count > r.busiestCount) {
            r.busiestCount = count;
            r.busiestLine = line;
        }
        if (count > 8) {
            if (!inOverflow) {
                r.overflows.push_back({line, line, count});
            }
            r.overflows.back().last = line;
            r.overflows.back().most = std::max(r.overflows.back().most, count);
        }
        inOverflow = count > 8;
    }
    for (const OamSprite &s : r.sprites) {
        r.dropped += s.droppedFirst >= 0 ? 1 : 0;
    }
    r.dmaSeen = rp2a03_debug_oam_dma(&r.dmaPage, &r.dmaFrame);
    r.hitSeen = ppu_debug_sprite0_hit(&r.hitLine, &r.hitX, &r.hitFrame);
    r.frame = ppu_debug_get_frame_num();
    return r;
}

std::string oamColumnTitles()
{
    return "## Y  X  Tile  P Pri Fl Lines   Notes";
}

std::vector<std::string> oamHeader(const OamReport &r)
{
    std::string setup = r.tall ? "8x16 (tile bit 0: table)" : format("8x8 at $%04X", r.table);
    if (r.dmaSeen) {
        // (frames are counted from the pre-render line: a copy in the vertical blank is "before this frame")
        std::string age = r.dmaFrame == r.frame       ? "during this frame"
                          : r.dmaFrame + 1 == r.frame ? "before this frame"
                                                      : format("%u frames ago", r.frame - r.dmaFrame);
        setup += format(", DMA from $%02X00 %s", r.dmaPage, age.c_str());
    } else {
        setup += ", no OAM DMA yet";
    }
    std::string warnings;
    if (!r.spritesOn) {
        warnings = "Sprites are off (PPUMASK).  ";
    }
    if (r.overflows.empty()) {
        warnings += format("%d visible, none dropped", r.visible);
        if (r.busiestCount > 0) {
            warnings += format(" (busiest: line %d, %d)", r.busiestLine, r.busiestCount);
        }
    } else {
        warnings += format("%d visible, %d dropped: ", r.visible, r.dropped);
        for (size_t i = 0; i < r.overflows.size() && i < 3; i++) {
            const OamReport::Overflow &o = r.overflows[i];
            warnings += (i ? ", " : "") +
                        (o.first == o.last ? format("line %d", o.first) : format("lines %d-%d", o.first, o.last)) +
                        format(" (%d)", o.most);
        }
        if (r.overflows.size() > 3) {
            warnings += ", ...";
        }
    }
    return {setup, warnings};
}

std::string oamNotes(const OamReport &r, int index)
{
    const OamSprite &s = r.sprites[static_cast<size_t>(index)];
    std::vector<std::string> notes;
    if (index == 0) {
        if (r.hitSeen && r.frame - r.hitFrame <= 1) {
            notes.push_back(format("sprite 0: hit at line %d x %d%s", r.hitLine, r.hitX,
                                   r.hitFrame == r.frame ? "" : " (last frame)"));
        } else {
            notes.emplace_back("sprite 0: no hit");
        }
    }
    if (s.hidden) {
        notes.emplace_back("hidden");
    }
    if (s.droppedFirst >= 0) {
        notes.push_back(s.droppedFirst == s.droppedLast
                            ? format("dropped on line %d", s.droppedFirst)
                            : format("dropped on lines %d-%d", s.droppedFirst, s.droppedLast));
    }
    if (!s.hidden && s.bottom > 239) {
        notes.emplace_back("partly below the picture");
    }
    if (s.leftMasked) {
        notes.emplace_back("left 8 pixels masked");
    }
    std::string text;
    for (const std::string &n : notes) {
        text += (text.empty() ? "" : "; ") + n;
    }
    return text;
}

std::string oamRow(const OamReport &r, int index)
{
    const OamSprite &s = r.sprites[static_cast<size_t>(index)];
    std::string tile = r.tall ? format("%02X/%02X", s.tile & 0xFE, (s.tile & 0xFE) + 1) : format("%02X", s.tile);
    std::string lines = s.hidden ? "-" : format("%d-%d", s.top, std::min(s.bottom, 239));
    char flips[3] = {(s.attr & 0x40) ? 'H' : '-', (s.attr & 0x80) ? 'V' : '-', 0};
    return format("%02d %02X %02X %-5s %d %-3s %s %-7s %s", index, s.y, s.x, tile.c_str(), s.palette(),
                  s.behind() ? "bg" : "fg", flips, lines.c_str(), oamNotes(r, index).c_str());
}
