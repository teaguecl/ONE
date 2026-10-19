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
#include "profiler_window.hpp"

#include <SDL.h>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "debugger/viewer_window.hpp"
#include "debugger/profiler.hpp"
#include "emulator.hpp"
#include "symbols.hpp"

namespace {
constexpr uint32_t kBackground = 0x202020, kText = 0xC0C0C0, kHead = 0x80C0FF, kSorted = 0xFFFF60, kDim = 0x808080,
                   kRoot = 0xA0E0A0;
constexpr int kLeft = 4, kPitch = 6, kRowHeight = 10, kTitleY = 4, kHeadY = 18, kListY = 30;

// Columns in characters: name, address, calls, incl, incl%, excl, excl%, avg, min, max
constexpr int kWidths[10] = {18, 9, 7, 9, 7, 9, 7, 7, 6, 6};

// "3 Calls": the key and the name
std::string columnLabel(int i)
{
    static const char *const labels[10] = {"1 Name", "2 Address", "3 Calls", "4 Incl", "5 Incl%",
                                           "6 Excl", "7 Excl%",   "8 Avg",   "9 Min",  "0 Max"};
    return labels[i];
}

std::string pad(std::string s, int width, bool right)
{
    if (static_cast<int>(s.size()) > width) {
        s.resize(static_cast<size_t>(width));
    }
    const std::string fill(static_cast<size_t>(width) - s.size(), ' ');
    return right ? fill + s : s + fill;
}

// A count in WIDTH characters: 123456789 -> "123457K" -> "123M" when it is too long
std::string number(double value, int width)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f", value);
    if (static_cast<int>(std::string(text).size()) > width) {
        const char *suffix = "KMGT";
        double v = value;
        int i = -1;
        do {
            v /= 1000.0;
            i++;
            std::snprintf(text, sizeof(text), "%.0f%c", v, suffix[i]);
        } while (static_cast<int>(std::string(text).size()) > width && i < 3);
    }
    return text;
}

std::string percent(uint64_t part, uint64_t whole)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%.1f",
                  whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole));
    return text;
}

const Symbols &noSymbols()
{
    static const Symbols none;
    return none;
}

int columnOfKey(int key)
{
    if (key >= SDLK_1 && key <= SDLK_9) {
        return key - SDLK_1;
    }
    return key == SDLK_0 ? 9 : -1;
}
} // namespace

ProfilerWindow &ProfilerWindow::instance()
{
    static ProfilerWindow window;
    return window;
}

ProfilerWindow::ProfilerWindow() : ViewerWindow("profiler", kWidth, kHeight, 2) {}

void ProfilerWindow::onOpened()
{
    Emulator::instance().startProfiling();
    top = 0;
}

void ProfilerWindow::onClosed()
{
    Emulator::instance().stopProfiling();
}

std::string ProfilerWindow::title() const
{
    const Profiler &profiler = const_cast<Emulator &>(Emulator::instance()).profiler();
    return std::string("ONE Debugger - Profiler - ") + std::to_string(profiler.functions()) + " functions";
}

void ProfilerWindow::sortBy(Profiler::Column c)
{
    if (c == column) {
        descending = !descending;
    } else {
        column = c;
        descending = c != Profiler::Column::Name && c != Profiler::Column::Address;
    }
    top = 0;
}

void ProfilerWindow::scrollBy(int rows)
{
    const int last = std::max(0, static_cast<int>(shown) - kRows);
    top = std::min(std::max(0, top + rows), last);
}

void ProfilerWindow::onKey(int key)
{
    const int c = columnOfKey(key);
    if (c >= 0) {
        sortBy(static_cast<Profiler::Column>(c));
    } else if (key == SDLK_LEFT || key == SDLK_RIGHT) {
        const int next = (static_cast<int>(column) + (key == SDLK_RIGHT ? 1 : 9)) % 10;
        sortBy(static_cast<Profiler::Column>(next));
    } else if (key == SDLK_f) {
        perFrame = !perFrame;
    } else if (key == SDLK_r) {
        Emulator::instance().profiler().clear();
        top = 0;
    } else if (key == SDLK_HOME) {
        top = 0;
    } else if (key == SDLK_END) {
        top = 1 << 30;
    } else {
        onKeyRepeat(key);
    }
    updateTitle();
    refresh();
}

void ProfilerWindow::onKeyRepeat(int key)
{
    switch (key) {
    case SDLK_UP: scrollBy(-1); break;
    case SDLK_DOWN: scrollBy(1); break;
    case SDLK_PAGEUP: scrollBy(-kRows + 1); break;
    case SDLK_PAGEDOWN: scrollBy(kRows - 1); break;
    default: return;
    }
    refresh();
}

std::string ProfilerWindow::header()
{
    std::string out;
    for (int i = 0; i < 10; i++) {
        const std::string label = columnLabel(i);
        out += pad(label, kWidths[i], i >= 2) + (i < 9 ? " " : "");
    }
    return out;
}

std::string ProfilerWindow::line(const Profiler::Row &row, uint64_t total, uint64_t frames, bool perFrame)
{
    const double divisor = perFrame && frames > 0 ? static_cast<double>(frames) : 1.0;
    auto cycles = [&](uint64_t v, int width) {
        return pad(number(static_cast<double>(v) / divisor, width), width, true);
    };
    std::string calls;
    if (row.isRoot) {
        calls = "";
    } else if (perFrame) {
        char text[24];
        std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(row.calls) / divisor);
        calls = text;
    } else {
        calls = number(static_cast<double>(row.calls), kWidths[2]);
    }
    std::string out = pad(row.name, kWidths[0], false) + " " + pad(row.address, kWidths[1], false) + " " +
                      pad(calls, kWidths[2], true) + " " + cycles(row.incl, kWidths[3]) + " " +
                      pad(percent(row.incl, total), kWidths[4], true) + " " + cycles(row.excl, kWidths[5]) + " " +
                      pad(percent(row.excl, total), kWidths[6], true) + " ";
    const bool counted = !row.isRoot && row.calls > 0;
    out += pad(counted ? number(static_cast<double>(Profiler::average(row)), kWidths[7]) : "", kWidths[7], true) + " ";
    out += pad(counted ? number(static_cast<double>(row.min), kWidths[8]) : "", kWidths[8], true) + " ";
    out += pad(counted ? number(static_cast<double>(row.max), kWidths[9]) : "", kWidths[9], true);
    return out;
}

void ProfilerWindow::render()
{
    Emulator &emu = Emulator::instance();
    Profiler &profiler = emu.profiler();
    std::vector<Profiler::Row> rows = profiler.rows(symbols != nullptr ? *symbols : noSymbols());
    const uint64_t total = profiler.totalCycles(), frames = profiler.frames();
    Profiler::sort(rows, column, descending);
    shown = rows.size();
    scrollBy(0);

    fill(0, 0, width, height, kBackground);
    char status[160];
    std::snprintf(status, sizeof(status), "%s  %zu routines  %llu frames  %llu cycles (%llu per frame)  %s",
                  profiler.running() ? "Profiling" : "Stopped", rows.size() - 1,
                  static_cast<unsigned long long>(frames), static_cast<unsigned long long>(total),
                  static_cast<unsigned long long>(frames == 0 ? 0 : total / frames), perFrame ? "per frame" : "totals");
    text(kLeft, kTitleY, status, kText);

    // The header, the sorted column's label brighter
    int x = kLeft;
    for (int i = 0; i < 10; i++) {
        const std::string label = columnLabel(i);
        text(x, kHeadY, pad(label, kWidths[i], i >= 2), i == static_cast<int>(column) ? kSorted : kHead);
        x += (kWidths[i] + 1) * kPitch;
    }
    fill(kLeft, kHeadY + 9, width - 2 * kLeft, 1, kDim);

    for (int i = 0; i < kRows && top + i < static_cast<int>(rows.size()); i++) {
        const Profiler::Row &row = rows[static_cast<size_t>(top) + static_cast<size_t>(i)];
        text(kLeft, kListY + i * kRowHeight, line(row, total, frames, perFrame), row.isRoot ? kRoot : kText);
    }
    if (rows.size() > static_cast<size_t>(kRows)) {
        const int trackH = kRows * kRowHeight;
        const int barH = std::max(6, trackH * kRows / static_cast<int>(rows.size()));
        const int barY = kListY + (trackH - barH) * top / std::max(1, static_cast<int>(rows.size()) - kRows);
        fill(width - 3, kListY, 1, trackH, 0x303030);
        fill(width - 4, barY, 3, barH, kDim);
    }
    text(kLeft, height - 11, "1-0/Left/Right sort  Up/Down/PgUp/PgDn scroll  F per frame  R reset", kDim);
}
