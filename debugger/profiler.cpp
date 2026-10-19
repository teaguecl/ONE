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
#include "profiler.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>

#include "core.hpp"
#include "debugger/flow.hpp"
#include "symbols.hpp"

uint64_t Profiler::now() const
{
    return cpu_debug_get_cycle();
}

uint64_t Profiler::keyOf(const CallFrame &frame)
{
    return (static_cast<uint64_t>(frame.targetOffset + 1) << 16) | frame.target; // (offset -1: not PRG-ROM)
}

void Profiler::start()
{
    if (active) {
        return;
    }
    active = true;
    stack.clear();
    lastCycle = now();
    startFrame = ppu_debug_get_frame_num();
    framesRunning = true;
}

void Profiler::stop()
{
    if (!active) {
        return;
    }
    account(now());
    framesDone += ppu_debug_get_frame_num() - startFrame;
    framesRunning = false;
    active = false;
    stack.clear();
}

void Profiler::clear()
{
    stats.clear();
    stack.clear();
    rootSelf = 0;
    framesDone = 0;
    lastCycle = now();
    startFrame = ppu_debug_get_frame_num();
}

void Profiler::account(uint64_t cycle)
{
    if (cycle > lastCycle) {
        (stack.empty() ? rootSelf : stack.back().self) += cycle - lastCycle;
    }
    lastCycle = cycle;
}

void Profiler::onEnter(const CallFrame &frame, uint64_t cycle)
{
    if (!active) {
        return;
    }
    account(cycle);
    const uint64_t key = keyOf(frame);
    Stat &stat = stats[key];
    stat.address = frame.target;
    stat.prgOffset = frame.targetOffset;
    stat.kind = frame.kind;
    stat.calls++;
    stat.depth++;
    Activation a;
    a.key = key;
    a.interrupt = frame.kind == CallFrame::Kind::Nmi || frame.kind == CallFrame::Kind::Irq;
    stack.push_back(a);
}

void Profiler::onLeave(const CallFrame &, uint64_t cycle)
{
    if (!active || stack.empty()) { // (nothing: the call was made before profiling began)
        return;
    }
    account(cycle);
    const Activation a = stack.back();
    stack.pop_back();
    const uint64_t incl = a.self + a.children;
    Stat &stat = stats[a.key];
    stat.excl += a.self;
    stat.depth--;
    if (stat.depth == 0) {
        stat.incl += incl;
    }
    if (!stat.returned) {
        stat.min = stat.max = incl;
        stat.returned = true;
    } else {
        stat.min = std::min(stat.min, incl);
        stat.max = std::max(stat.max, incl);
    }
    if (!stack.empty() && !a.interrupt) {
        stack.back().children += incl;
    }
}

void Profiler::onDrop()
{
    if (active && !stack.empty()) { // the oldest activation: its time is lost, the ones above it go on
        Stat &stat = stats[stack.front().key];
        stat.excl += stack.front().self;
        stat.depth--;
        stack.erase(stack.begin());
    }
}

void Profiler::onReset()
{
    if (!active) {
        return;
    }
    for (const Activation &a : stack) { // (what they have used so far is kept as their own time)
        Stat &stat = stats[a.key];
        stat.excl += a.self;
        stat.incl += a.self + a.children;
        stat.depth = 0;
    }
    stack.clear();
    lastCycle = now(); // (the time that went by is not known: a state load can even go back)
}

uint64_t Profiler::frames() const
{
    return framesDone + (framesRunning ? ppu_debug_get_frame_num() - startFrame : 0);
}

std::string Profiler::addressText(uint16_t address, int32_t prgOffset)
{
    char text[24];
    size_t size = 0;
    uint32_t bankSize = 0;
    if (prgOffset >= 0 && mapper_debug_prg_rom(&size) != nullptr && size != 0) {
        for (uint32_t at = 0x8000; at <= 0xFFFF && bankSize == 0; at += 0x2000) {
            int bank = 0, banks = 0;
            if (mapper_debug_prg_bank(static_cast<uint16_t>(at), &bank, &banks) && banks > 0) {
                bankSize = static_cast<uint32_t>(size / static_cast<size_t>(banks));
            }
        }
    }
    if (prgOffset >= 0 && bankSize != 0) {
        std::snprintf(text, sizeof(text), "%02X:$%04X", static_cast<unsigned>(prgOffset) / bankSize, address);
    } else {
        std::snprintf(text, sizeof(text), "$%04X", address);
    }
    return text;
}

std::vector<Profiler::Row> Profiler::rows(const Symbols &symbols) const
{
    // The figures as of now: what the running routines have used is added as if they returned this instant
    std::unordered_map<uint64_t, Stat> shown = stats;
    uint64_t root = rootSelf;
    if (active) {
        const uint64_t cycle = now();
        const uint64_t extra = cycle > lastCycle ? cycle - lastCycle : 0;
        std::vector<uint64_t> incl(stack.size(), 0);
        for (size_t i = stack.size(); i-- > 0;) {
            const Activation &a = stack[i];
            const bool top = i + 1 == stack.size();
            const uint64_t self = a.self + (top ? extra : 0);
            incl[i] = self + a.children + (top || stack[i + 1].interrupt ? 0 : incl[i + 1]);
            shown[a.key].excl += self;
        }
        std::unordered_map<uint64_t, bool> seen; // (recursion: Incl from the lowest activation of a routine only)
        for (size_t i = 0; i < stack.size(); i++) {
            if (!seen[stack[i].key]) {
                seen[stack[i].key] = true;
                shown[stack[i].key].incl += incl[i];
            }
        }
        if (stack.empty()) {
            root += extra;
        }
    }
    std::vector<Row> result;
    for (const auto &entry : shown) {
        const Stat &stat = entry.second;
        Row row;
        row.cpuAddress = stat.address;
        row.prgOffset = stat.prgOffset;
        row.address = addressText(stat.address, stat.prgOffset);
        const std::string *label = nullptr;
        if (stat.prgOffset >= 0) {
            label = symbols.findAt({Symbols::Kind::PrgRom, static_cast<uint32_t>(stat.prgOffset)});
        } else {
            label = symbols.find(stat.address);
        }
        if (label != nullptr) {
            row.name = *label;
        } else {
            switch (stat.kind) {
            case CallFrame::Kind::Nmi: row.name = "NMI"; break;
            case CallFrame::Kind::Irq: row.name = "IRQ"; break;
            case CallFrame::Kind::Brk: row.name = "BRK"; break;
            case CallFrame::Kind::Reset: row.name = "Reset"; break;
            case CallFrame::Kind::Call: break;
            }
        }
        row.calls = stat.calls;
        row.incl = stat.incl;
        row.excl = stat.excl;
        row.min = stat.returned ? stat.min : 0;
        row.max = stat.returned ? stat.max : 0;
        result.push_back(row);
    }
    Row r;
    r.name = "[root]";
    r.isRoot = true;
    r.excl = r.incl = root;
    result.push_back(r);
    return result;
}

uint64_t Profiler::totalCycles() const
{
    uint64_t total = rootSelf;
    for (const auto &entry : stats) {
        total += entry.second.excl;
    }
    for (const Activation &a : stack) {
        total += a.self;
    }
    if (active) {
        const uint64_t cycle = now();
        total += cycle > lastCycle ? cycle - lastCycle : 0;
    }
    return total;
}

const char *Profiler::columnName(Column column)
{
    static const char *const names[] = {"Name", "Address", "Calls", "Incl", "Incl%",
                                        "Excl", "Excl%",   "Avg",   "Min",  "Max"};
    return names[static_cast<int>(column)];
}

void Profiler::sort(std::vector<Row> &table, Column column, bool descending)
{
    // (the percentages sort like the cycle counts they come from)
    auto key = [&](const Row &r) -> uint64_t {
        switch (column) {
        case Column::Calls: return r.calls;
        case Column::Incl:
        case Column::InclPercent: return r.incl;
        case Column::Excl:
        case Column::ExclPercent: return r.excl;
        case Column::Avg: return average(r);
        case Column::Min: return r.min;
        case Column::Max: return r.max;
        default: return 0;
        }
    };
    auto less = [&](const Row &a, const Row &b) {
        if (column == Column::Name) {
            if (a.name.empty() != b.name.empty()) {
                return b.name.empty(); // unnamed last (reversed with the order, like the rest)
            }
            return a.name != b.name ? a.name < b.name : a.cpuAddress < b.cpuAddress;
        }
        if (column == Column::Address) { // RAM code by address, then PRG-ROM by bank and address (its offset)
            auto place = [](const Row &r) -> uint64_t {
                return r.isRoot           ? ~0ULL
                       : r.prgOffset >= 0 ? 0x1000000ULL + static_cast<uint64_t>(r.prgOffset)
                                          : r.cpuAddress;
            };
            return place(a) < place(b);
        }
        return key(a) != key(b) ? key(a) < key(b) : a.cpuAddress < b.cpuAddress;
    };
    std::stable_sort(table.begin(), table.end(),
                     [&](const Row &a, const Row &b) { return descending ? less(b, a) : less(a, b); });
}
