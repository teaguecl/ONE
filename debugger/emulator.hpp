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
// Run control for the debugger: stepping, running in slices, breakpoints. Wraps the C core's debug hooks
// (debug_hooks.h). There is one emulator, so this is a singleton (the C hooks are plain functions).
#pragma once
#include <array>
#include <optional>
#include <cstdint>
#include <string>
#include <vector>
#include "cdl.hpp"
#include "core.hpp"
#include "flow.hpp"
#include "profiler.hpp"
#include "search.hpp"

struct Breakpoint {
    enum class Type { Execute, Read, Write, ReadWrite, Opcode };
    Type type = Type::Execute;
    uint16_t address = 0; // Opcode: the opcode
    bool enabled = true;
    std::string condition; // bpx ... if EXPRESSION: Lua, stops only when true (script_condition_eval)
    int conditionHandle = -1;
    std::string action; // bpx ... do "COMMANDS": debugger commands run after it stops (';' between them)
};

// A debugger key pressed in the game window (F5, F8, F10, F12, kCtrlD, kAltX)
struct GameKey {
    static constexpr int kCtrlD = DBG_KEY_CTRL_D;
    static constexpr int kQuit = DBG_KEY_QUIT; // the game window's close button
    static constexpr int kAltX = DBG_KEY_ALT_X;
    int key;
    bool whileRunning; // the key stopped a run (so F5 / Ctrl+D have done their job already)
};

enum class StopReason {
    None,       // still running (slice ended at a frame boundary) -- not a stop
    Step,       // single step / step over / go-to address finished
    Return,     // 'p ret' reached RTS/RTI
    Breakpoint, // hitBreakpoint() says which
    Scanline,   // gsl reached its scanline
    Frames,     // fs: the frames ran (stopped at the start of vblank)
    UserBreak,  // stop() while running
    Jammed,     // the CPU executed a JAM (or unimplemented) opcode; jamOpcode() says which
    Ended,      // the emulation ended (window closed, ...); power on again to continue
};

class Emulator {
public:
    static Emulator &instance();

    bool powerOn(const std::string &romFile); // power on and stop at the first instruction (reset vector)
    bool powerCycle();                        // 'reset hard'
    void softReset();                         // 'reset': RESET button, stops at the reset vector
    bool ended() const { return hasEnded; }
    bool hasRom() const { return !rom.empty(); } // a game is loaded (one-debugger can start without one)
    const std::string &romPath() const { return rom; }

    // Execution control. step*/stepOver/stepToReturn complete before returning (with a safety limit);
    // run*/goTo*/runToScanline only arm the run -- call runSlice() repeatedly (UI idle loop) while running().
    StopReason stepInto(unsigned count = 1);
    StopReason stepOver();
    StopReason stepToReturn();
    void run();
    void goTo(uint16_t address);
    void runToScanline(unsigned scanline);
    void runFrames(unsigned count); // fs: run until vblank has started COUNT times (stops at the start of vblank)
    bool running() const { return isRunning; }
    bool insideCpu() const { return inCpu; } // the emulation is executing (a Lua callback is running inside it)
    void stop() { stopRequested = true; }
    StopReason runSlice(); // runs until the next frame or a stop

    // Breakpoints. Returns the slot number, or -1 if the table is full.
    // A condition (Lua expression) or action replaces the breakpoint's earlier one; conditionHandle comes from
    // script_condition_compile() and belongs to the breakpoint from then on.
    int addBreakpoint(Breakpoint::Type type, uint16_t address, const std::string &condition = {},
                      int conditionHandle = -1, const std::string &action = {});
    bool removeBreakpoint(unsigned slot);
    void removeAllBreakpoints();
    bool setBreakpointEnabled(unsigned slot, bool enabled);
    const std::vector<Breakpoint> &breakpoints() const { return bps; }
    int breakpointAt(uint16_t address) const; // execute breakpoint slot at address, -1 if none
    int hitBreakpoint() const { return lastHit; }
    uint16_t hitAccessAddress() const { return lastHitAddress; }
    bool hitAccessWasWrite() const { return lastHitWrite; }
    uint8_t jamOpcode() const { return jammedOpcode; }

    // Memory search (cs commands): CPU reads/writes are reported to it while a CPU search runs
    MemorySearch &search() { return memorySearch; }

    // Freeze: these bytes are written back once per frame (and when set), so the game cannot change them
    struct Freeze {
        uint16_t address;
        uint8_t value;
    };
    bool freeze(uint16_t address, uint8_t value); // false if the address is not RAM
    bool unfreeze(uint16_t address);
    void unfreezeAll() { freezes.clear(); }
    const std::vector<Freeze> &frozen() const { return freezes; }

    // Keys pressed in the game window: a run stops at the next instruction, and the UI then acts on the key
    std::optional<GameKey> takeGameKey();

    static constexpr unsigned kMaxBreakpoints = 16;

    // Instructions the CPU has started at, since the ROM was loaded (PRG-ROM by its offset, so each bank of
    // banked code is remembered separately; RAM by address): the disassembler trusts these over its guesses.
    bool executedAt(uint16_t address) const;

    // How the CPU got here: the call stack (bt) and, when switched on (cdl on), the code/data log
    // The call tracking costs ~8% per instruction, so it starts with the first bt (or cdl on), not before: calls made
    // earlier are not known (bt guesses them from the stack)
    FlowTracker &flowTracker() { return flow; }
    bool flowTracking() const { return flowUsers != 0; }
    void startFlowTracking(); // for bt and cdl: stays on
    // The profiler (prof, wprof): it needs the flow tracking while it runs, and turns it off again when it stops
    // (unless bt / cdl asked for it). Nothing of it runs while it is off.
    Profiler &profiler() { return prof; }
    void startProfiling();
    void stopProfiling();
    CodeDataLog &codeLog() { return cdl; }

private:
    void noteExecuted(uint16_t pc);
    FlowTracker flow;
    unsigned flowUsers = 0;  // bit 0: bt / cdl, bit 1: the profiler
    bool flowActive = false; // flowUsers != 0, as atBoundary reads it
    Profiler prof;
    CodeDataLog cdl;
    std::vector<bool> executedRom; // by PRG-ROM offset
    std::vector<bool> executedLow; // $0000-$7FFF by address (RAM, cartridge RAM)
    Emulator() = default;
    enum class Mode { Stopped, Step, StepOver, Return, Run, Scanline, Frames };

    StopReason resume(Mode mode); // run until the boundary hook pauses
    void rebuildBreakpointIndex();
    bool atBoundary(uint16_t pc);
    void onAccess(uint16_t address, uint8_t value, bool write);
    static bool boundaryHook(uint16_t pc);
    static void accessHook(uint16_t address, uint8_t value, bool write);
    static void jamHook(uint16_t pc, uint8_t opcode);
    static void dmcHook(uint16_t address);
    static void keyHook(int key, bool repeat);

    std::string rom;
    bool hasEnded = false;
    bool isRunning = false;
    bool stopRequested = false;
    bool inCpu = false;
    bool conditionMet(const Breakpoint &bp, int address, int value) const;

    Mode mode = Mode::Stopped;
    StopReason stopReason = StopReason::None;
    unsigned stepsLeft = 0;
    int tempAddress = -1;         // go-to / step-over return address
    unsigned minStackPointer = 0; // step over / p ret: only stop in this stack frame or an outer one
    bool returnTaken = false;     // Mode::Return: the RTS / RTI has been reached, the next boundary is the caller's
    int targetScanline = -1;
    unsigned framesLeft = 0;       // fs: vblank starts still to pass
    uint32_t previousScanline = 0; // fs: the scanline at the previous instruction (to see 241 begin)
    uint32_t sliceFrame = 0;

    std::vector<Breakpoint> bps;
    std::array<uint8_t, 0x10000> execIndex{};   // execute breakpoints by address
    std::array<uint8_t, 0x10000> accessIndex{}; // bit0 read, bit1 write
    std::array<uint8_t, 0x100> opcodeIndex{};
    bool accessHitPending = false;
    int lastHit = -1;
    uint16_t lastHitAddress = 0;
    bool lastHitWrite = false;
    uint8_t jammedOpcode = 0;
    std::optional<GameKey> pendingKey;
    MemorySearch memorySearch;
    std::vector<Freeze> freezes;
    uint32_t freezeFrame = 0xFFFFFFFF;
    uint16_t instructionPc = 0; // the instruction being executed (for the search's "last writer")
};
