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
// Control flow as the CPU runs, for the backtrace (bt) and the code/data log (cdl.hpp): a shadow call stack and how each
// instruction was reached. It is fed at every instruction boundary (Emulator::atBoundary) and needs no core change.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

// How the instruction at the current boundary was reached from the one before it
enum class Arrival : uint8_t {
    Sequential,   // the next instruction (or a branch not taken)
    Call,         // JSR
    Jump,         // JMP absolute, or a taken branch
    JumpIndirect, // JMP (ind)
    Return,       // RTS / RTI back to where the call was made (or unknown: nothing was recorded)
    Dispatch,     // RTS to somewhere else than the caller: a jump table using the return address
    Interrupt,    // NMI, IRQ or BRK
    Reset,        // the reset vector
};

// A call, interrupt or BRK that has not returned yet
struct CallFrame {
    enum class Kind : uint8_t { Call, Nmi, Irq, Brk, Reset };
    Kind kind = Kind::Call;
    uint16_t callSite = 0;       // JSR: the JSR; interrupts: the instruction that was about to run
    uint16_t target = 0;         // where the routine starts
    uint16_t returnAddress = 0;  // where RTS / RTI goes back to
    uint8_t entrySp = 0;         // SP when the routine started (the frame is gone once SP is above this)
    int32_t callSiteOffset = -1; // PRG-ROM offsets at the time (so the bank is right whichever one is mapped later)
    int32_t targetOffset = -1;
};

// A jump or taken branch, for "bt jumps"
struct Transfer {
    uint16_t from = 0;
    uint16_t to = 0;
    uint8_t opcode = 0;
    int32_t fromOffset = -1;
    int32_t toOffset = -1;
};

// Told about the calls and returns the tracker sees (the profiler: profiler.hpp). CYCLE is the CPU cycle count at the
// instruction boundary where it was noticed.
class FlowListener {
public:
    virtual ~FlowListener() = default;
    virtual void onEnter(const CallFrame &frame, uint64_t cycle) = 0; // a routine or interrupt handler starts
    virtual void onLeave(const CallFrame &frame, uint64_t cycle) = 0; // its frame is gone (innermost first)
    virtual void onDrop() = 0;  // the oldest frame was forgotten (a runaway recursion): no return will be seen
    virtual void onReset() = 0; // the stack was cleared: a state load, a reset, the PC changed by the debugger
};

class FlowTracker {
public:
    // At most one listener (nullptr: none); the tracker does not own it
    void setListener(FlowListener *l) { listener = l; }

    // The CPU is about to run the instruction at PC (SP, the cycle count and the opcode are read from the CPU)
    void onBoundary(uint16_t pc);
    // Forget everything (a game was loaded, reset, a state was loaded)
    void reset();

    Arrival arrival() const { return lastArrival; }
    // Innermost last. Empty after a reset or a state load until the next call: see Backtrace guessing in commands.cpp
    const std::vector<CallFrame> &frames() const { return stack; }
    const std::deque<Transfer> &transfers() const { return recent; }
    // The most recent jump / taken branch / dispatch if it went to PC, else nullptr
    const Transfer *reachedBy(uint16_t pc) const;
    // The opcode that made the last jump (valid after Jump, JumpIndirect, Dispatch)
    uint8_t previousOpcode() const { return prevOpcode; }
    uint16_t previousPc() const { return prevPc; }

    static constexpr size_t kMaxFrames = 256;
    static constexpr size_t kMaxTransfers = 16;

private:
    void push(CallFrame::Kind kind, uint16_t callSite, uint16_t target, uint16_t ret, uint8_t sp);

    uint16_t predictNext(uint16_t pc, uint8_t opcode, uint8_t sp) const;

    FlowListener *listener = nullptr;
    uint64_t nowCycle = 0; // the boundary being handled
    bool havePrevious = false;
    uint16_t prevPc = 0;
    uint8_t prevOpcode = 0;
    uint8_t prevSize = 1;
    uint16_t prevPredicted = 0; // where the previous instruction should send the CPU
    uint64_t prevCycle = 0;
    Arrival lastArrival = Arrival::Sequential;
    std::vector<CallFrame> stack;
    std::deque<Transfer> recent;
};
