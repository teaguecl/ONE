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
// The profiler (commands prof and wprof, after Mesen's): for each routine, how often it was called and how many CPU
// cycles it took. It listens to the FlowTracker (flow.hpp), which sees every JSR / RTS / RTI and interrupt, so it costs
// nothing while it is off (the emulator has no profiler then) and needs no change in the core.
//
// Time is charged to the routine on top of the call stack ("self"). From that:
//   Excl  = self time, the routine's own cycles
//   Incl  = self + the Incl of the routines it called; an NMI / IRQ that arrived meanwhile is not counted in the
//           routine it interrupted (neither Incl nor Excl): it has its own row
//   [root] = the time with no routine on the stack (the main loop when it is not inside a JSR)
// so the Excl column (with [root]) adds up to all the cycles profiled. A routine still running (the main loop that
// never returns, or a stop inside a routine) is counted up to now in the figures shown, but not in Min / Max.
// Recursion: Incl counts the outermost activation only. A JMP into another routine (tail call) or an RTS jump table
// stays charged to the routine entered by JSR, as in bt.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "flow.hpp"

class Symbols;

class Profiler : public FlowListener {
public:
    struct Row {
        std::string
            name; // the label, "NMI" / "IRQ" / "BRK" / "Reset" for a handler without one, or "" (root: "[root]")
        std::string address;    // "$C123" or "03:$C123" (banked), "" for the root
        int32_t prgOffset = -1; // the sort key of the address: bank, then address
        uint16_t cpuAddress = 0;
        uint64_t calls = 0;
        uint64_t incl = 0, excl = 0;
        uint64_t min = 0, max = 0; // of one call's Incl (0, 0 until a call has returned)
        bool isRoot = false;
    };
    // The columns a table can be sorted by (the window's keys 1-9, 0)
    enum class Column { Name, Address, Calls, Incl, InclPercent, Excl, ExclPercent, Avg, Min, Max };

    void start(); // begin counting (flow tracking must be on); the counts so far are kept
    void stop();  // stop; the counts stay until clear()
    void clear(); // forget the counts (profiling goes on if it was on)
    bool running() const { return active; }

    // The rows as of now, unsorted; the figures include the routines still running
    std::vector<Row> rows(const Symbols &symbols) const;
    uint64_t totalCycles() const; // the cycles profiled (all the rows' Excl together)
    uint64_t frames() const;      // the frames profiled
    size_t functions() const { return stats.size(); }

    static void sort(std::vector<Row> &table, Column column, bool descending);
    static const char *columnName(Column column);
    // "03:$C123"
    static std::string addressText(uint16_t address, int32_t prgOffset);
    static uint64_t average(const Row &row) { return row.calls == 0 ? 0 : row.incl / row.calls; }

    // FlowListener
    void onEnter(const CallFrame &frame, uint64_t cycle) override;
    void onLeave(const CallFrame &frame, uint64_t cycle) override;
    void onDrop() override;
    void onReset() override;

private:
    struct Stat {
        uint16_t address = 0;
        int32_t prgOffset = -1;
        CallFrame::Kind kind = CallFrame::Kind::Call;
        uint64_t calls = 0, incl = 0, excl = 0, min = 0, max = 0;
        uint32_t depth = 0;
        bool returned = false; // min / max valid
    };
    struct Activation {
        uint64_t key = 0;
        uint64_t self = 0;     // cycles with this routine on top
        uint64_t children = 0; // Incl of the routines it called (not the interrupts)
        bool interrupt = false;
    };
    static uint64_t keyOf(const CallFrame &frame);
    void account(uint64_t cycle); // charge the time since the last event to what is on top
    uint64_t now() const;

    bool active = false;
    std::unordered_map<uint64_t, Stat> stats;
    std::vector<Activation> stack;
    uint64_t lastCycle = 0;
    uint64_t rootSelf = 0;
    uint64_t startFrame = 0, framesDone = 0;
    bool framesRunning = false;
};
