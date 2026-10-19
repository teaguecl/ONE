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
#include "emulator.hpp"
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include "core.hpp"
#include "disasm.hpp"

namespace {
// Safety limit for the synchronous stepping commands (step over a subroutine that never returns, ...):
// about 10 seconds of NES time. The user can always use 'go' to run it in the background instead.
constexpr unsigned kStepInstructionLimit = 2'000'000;
}

Emulator &Emulator::instance()
{
    static Emulator emulator;
    return emulator;
}

bool Emulator::boundaryHook(uint16_t pc)
{
    return instance().atBoundary(pc);
}

void Emulator::accessHook(uint16_t address, uint8_t value, bool write)
{
    instance().onAccess(address, value, write);
}

void Emulator::dmcHook(uint16_t address)
{
    Emulator &emu = instance();
    if (emu.cdl.enabled()) {
        emu.cdl.onDmcRead(address);
    }
}

void Emulator::jamHook(uint16_t /*pc*/, uint8_t opcode)
{
    Emulator &emu = instance();
    emu.jammedOpcode = opcode;
    emu.stopReason = StopReason::Jammed;
}

void Emulator::keyHook(int key, bool repeat)
{
    Emulator &emu = instance();
    if (emu.isRunning) {
        if (repeat) {
            return; // held down from before the run: not a new request
        }
        emu.stopRequested = true; // break at the next instruction boundary
        emu.pendingKey = GameKey{key, true};
    } else if (!repeat || key != 5) { // holding F8/F10 keeps stepping; holding F5 does not restart
        emu.pendingKey = GameKey{key, false};
    }
}

bool Emulator::freeze(uint16_t address, uint8_t value)
{
    if (!cpu_debug_write(address, value)) { // also applies it right away
        return false;
    }
    for (Freeze &f : freezes) {
        if (f.address == address) {
            f.value = value;
            return true;
        }
    }
    freezes.push_back({address, value});
    return true;
}

bool Emulator::unfreeze(uint16_t address)
{
    for (size_t i = 0; i < freezes.size(); i++) {
        if (freezes[i].address == address) {
            freezes.erase(freezes.begin() + static_cast<long>(i));
            return true;
        }
    }
    return false;
}

std::optional<GameKey> Emulator::takeGameKey()
{
    std::optional<GameKey> key = pendingKey;
    pendingKey.reset();
    return key;
}

bool Emulator::powerOn(const std::string &romFile)
{
    if (romFile != rom) { // another game: what ran where says nothing about it
        executedRom.clear();
        executedLow.clear();
        cdl.discard();
        prof.clear();
    }
    flow.reset();
    rom = romFile;
    isRunning = false;
    if (!emu_power_on(const_cast<char *>(rom.c_str()))) {
        hasEnded = true;
        rom.clear(); // (no game now: the old one was switched off)
        return false;
    }
    hasEnded = false;
    dbg_set_hooks(&Emulator::boundaryHook, &Emulator::accessHook, &Emulator::jamHook);
    dbg_set_key_hook(&Emulator::keyHook);
    dbg_set_dmc_hook(&Emulator::dmcHook);
    stepsLeft = 1;
    resume(Mode::Step); // run the reset sequence; stop at the first instruction
    return !hasEnded;
}

bool Emulator::powerCycle()
{
    isRunning = false;
    return powerOn(rom);
}

void Emulator::softReset()
{
    isRunning = false;
    flow.reset();
    cpu_request_soft_reset();
    stepsLeft = 1;
    resume(Mode::Step); // the reset sequence runs, then we stop at the reset vector
}

StopReason Emulator::resume(Mode newMode)
{
    if (hasEnded) {
        return StopReason::Ended;
    }
    mode = newMode;
    stopReason = StopReason::None;
    lastHit = -1;
    accessHitPending = false;
    sliceFrame = ppu_debug_get_frame_num();
    inCpu = true;
    bool resumed = dbg_resume();
    inCpu = false;
    if (!resumed) {
        hasEnded = true;
        isRunning = false;
        return StopReason::Ended;
    }
    return stopReason;
}

void Emulator::startFlowTracking()
{
    if (flowUsers == 0) {
        flow.reset();
    }
    flowUsers |= 1;
    flowActive = true;
}

void Emulator::startProfiling()
{
    if (flowUsers == 0) {
        flow.reset();
    }
    flowUsers |= 2;
    flowActive = true;
    flow.setListener(&prof);
    prof.start();
}

void Emulator::stopProfiling()
{
    prof.stop();
    flow.setListener(nullptr);
    flowUsers &= ~2u;
    flowActive = flowUsers != 0;
    if (!flowActive) {
        flow.reset();
    }
}

// Called by the core at every instruction boundary. Return true to pause.
bool Emulator::atBoundary(uint16_t pc)
{
    instructionPc = pc;
    noteExecuted(pc);
    if (flowActive) {
        flow.onBoundary(pc);
    }
    if (cdl.enabled()) {
        cdl.onInstruction(pc, cpu_debug_read(pc), flow.arrival());
    }
    if (memorySearch.active()) {
        memorySearch.beforeRun();
    }
    if (!freezes.empty()) {
        uint32_t frame = ppu_debug_get_frame_num();
        if (frame != freezeFrame) { // once per frame, like a cheat device
            freezeFrame = frame;
            for (const Freeze &f : freezes) {
                cpu_debug_write(f.address, f.value);
            }
        }
    }
    auto stopWith = [this](StopReason reason) {
        stopReason = reason;
        return true;
    };

    // Breakpoints apply in every mode except a plain single step
    if (mode != Mode::Step) {
        if (accessHitPending) {
            return stopWith(StopReason::Breakpoint);
        }
        if (execIndex[pc]) {
            for (unsigned i = 0; i < bps.size(); i++) {
                const Breakpoint &bp = bps[i];
                if (bp.enabled && bp.type == Breakpoint::Type::Execute && bp.address == pc &&
                    conditionMet(bp, -1, -1)) {
                    lastHit = static_cast<int>(i);
                    return stopWith(StopReason::Breakpoint);
                }
            }
        }
        if (opcodeIndex[cpu_debug_read(pc)]) {
            uint8_t opcode = cpu_debug_read(pc);
            for (unsigned i = 0; i < bps.size(); i++) {
                const Breakpoint &bp = bps[i];
                if (bp.enabled && bp.type == Breakpoint::Type::Opcode && bp.address == opcode &&
                    conditionMet(bp, -1, -1)) {
                    lastHit = static_cast<int>(i);
                    return stopWith(StopReason::Breakpoint);
                }
            }
        }
    }

    switch (mode) {
    case Mode::Step:
        if (--stepsLeft == 0) {
            return stopWith(StopReason::Step);
        }
        break;
    case Mode::StepOver:
        if (pc == tempAddress && cpu_debug_get_sp() >= minStackPointer) {
            return stopWith(StopReason::Step);
        }
        break;
    case Mode::Return: // (SoftICE's P RET): the return runs, and the stop is in the caller
        if (returnTaken) {
            return stopWith(StopReason::Return);
        }
        if (Disassembler::isReturn(cpu_debug_read(pc)) && cpu_debug_get_sp() >= minStackPointer) {
            returnTaken = true; // this instruction is the RTS / RTI: stop at the next one
        }
        break;
    case Mode::Scanline:
        if (ppu_debug_get_scanline() == static_cast<uint32_t>(targetScanline)) {
            return stopWith(StopReason::Scanline);
        }
        break;
    case Mode::Frames: {
        uint32_t line = ppu_debug_get_scanline();
        if (line == 241 && previousScanline != 241 && --framesLeft == 0) {
            return stopWith(StopReason::Frames);
        }
        previousScanline = line;
        break;
    }
    case Mode::Run:
        if (pc == tempAddress) {
            return stopWith(StopReason::Step);
        }
        break;
    case Mode::Stopped: return stopWith(StopReason::UserBreak);
    }

    // synchronous commands (step over, p ret) give up after kStepInstructionLimit instructions
    if (mode == Mode::StepOver || mode == Mode::Return) {
        if (++stepsLeft >= kStepInstructionLimit) {
            return stopWith(StopReason::UserBreak);
        }
    }
    // Background runs hand control back to the UI once per frame
    if (isRunning) {
        if (stopRequested) {
            stopRequested = false;
            return stopWith(StopReason::UserBreak);
        }
        if (ppu_debug_get_frame_num() != sliceFrame) {
            return stopWith(StopReason::None);
        }
    }
    return false;
}

void Emulator::noteExecuted(uint16_t pc)
{
    if (pc < 0x8000) {
        if (executedLow.empty()) {
            executedLow.assign(0x8000, false);
        }
        executedLow[pc] = true;
        return;
    }
    int32_t offset = mapper_debug_prg_offset(pc);
    if (offset < 0) {
        return;
    }
    if (static_cast<size_t>(offset) >= executedRom.size()) {
        executedRom.resize(static_cast<size_t>(offset) + 0x4000, false);
    }
    executedRom[static_cast<size_t>(offset)] = true;
}

bool Emulator::executedAt(uint16_t address) const
{
    if (address < 0x8000) {
        return address < executedLow.size() && executedLow[address];
    }
    int32_t offset = mapper_debug_prg_offset(address);
    return offset >= 0 && static_cast<size_t>(offset) < executedRom.size() && executedRom[static_cast<size_t>(offset)];
}

void Emulator::onAccess(uint16_t address, uint8_t value, bool write)
{
    if (!write && cdl.enabled()) {
        cdl.onRead(address);
    }
    if (memorySearch.active() && memorySearch.target() == MemorySearch::Target::Cpu) {
        memorySearch.onCpuAccess(address, write, instructionPc);
    }
    uint8_t want = accessIndex[address];
    if (want && (want & (write ? 2 : 1)) && !accessHitPending && mode != Mode::Step) {
        for (unsigned i = 0; i < bps.size(); i++) {
            const Breakpoint &bp = bps[i];
            if (!bp.enabled || bp.address != address) {
                continue;
            }
            if (((bp.type == Breakpoint::Type::ReadWrite) || (bp.type == Breakpoint::Type::Read && !write) ||
                 (bp.type == Breakpoint::Type::Write && write)) &&
                conditionMet(bp, address, value)) {
                accessHitPending = true;
                lastHitAddress = address;
                lastHitWrite = write;
                lastHit = static_cast<int>(i);
                break;
            }
        }
    }
}

bool Emulator::conditionMet(const Breakpoint &bp, int address, int value) const
{
    return bp.conditionHandle < 0 || script_condition_eval(bp.conditionHandle, address, value);
}

StopReason Emulator::stepInto(unsigned count)
{
    isRunning = false;
    if (count == 0) {
        return StopReason::Step;
    }
    stepsLeft = count;
    return resume(Mode::Step);
}

StopReason Emulator::stepOver()
{
    isRunning = false;
    uint16_t pc = cpu_debug_get_pc();
    uint8_t opcode = cpu_debug_read(pc);
    if (!Disassembler::isCall(opcode)) {
        return stepInto(1);
    }
    tempAddress = static_cast<uint16_t>(pc + Disassembler::size(opcode));
    minStackPointer = cpu_debug_get_sp(); // stop at the return address in this stack frame (not in a recursion)
    stepsLeft = 0;
    StopReason reason = resume(Mode::StepOver);
    tempAddress = -1;
    return reason;
}

StopReason Emulator::stepToReturn()
{
    isRunning = false;
    // On an RTS / RTI already: it is the one to run. Else the RTS/RTI of this subroutine, not of the ones it calls
    returnTaken = Disassembler::isReturn(cpu_debug_read(cpu_debug_get_pc()));
    minStackPointer = cpu_debug_get_sp();
    stepsLeft = 0;
    return resume(Mode::Return);
}

void Emulator::run()
{
    tempAddress = -1;
    isRunning = true;
    stopRequested = false;
    mode = Mode::Run;
}

void Emulator::goTo(uint16_t address)
{
    run();
    tempAddress = address;
}

void Emulator::runFrames(unsigned count)
{
    isRunning = true;
    stopRequested = false;
    tempAddress = -1;
    framesLeft = count;
    previousScanline = ppu_debug_get_scanline(); // (stopped in vblank: the next one counts)
    mode = Mode::Frames;
}

void Emulator::runToScanline(unsigned scanline)
{
    isRunning = true;
    stopRequested = false;
    tempAddress = -1;
    targetScanline = static_cast<int>(scanline);
    mode = Mode::Scanline;
}

StopReason Emulator::runSlice()
{
    if (!isRunning) {
        return StopReason::Step;
    }
    // Run whole frames until ~15 ms have passed, then let the UI look at the keyboard. At NES speed (throttled)
    // that is one frame per slice; unthrottled or without a game window it is several.
    const auto start = std::chrono::steady_clock::now();
    const Mode runMode = mode;
    StopReason reason;
    do {
        reason = resume(runMode);
    } while (reason == StopReason::None && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(15));
    if (reason != StopReason::None) {
        isRunning = false;
        tempAddress = -1;
        targetScanline = -1;
    }
    return reason;
}

int Emulator::addBreakpoint(Breakpoint::Type type, uint16_t address, const std::string &condition, int conditionHandle,
                            const std::string &action)
{
    for (unsigned i = 0; i < bps.size(); i++) {
        Breakpoint &bp = bps[i];
        if (bp.type == type && bp.address == address) {
            bp.enabled = true;
            script_condition_free(bp.conditionHandle);
            bp.condition = condition;
            bp.conditionHandle = conditionHandle;
            bp.action = action;
            rebuildBreakpointIndex();
            return static_cast<int>(i);
        }
    }
    if (bps.size() >= kMaxBreakpoints) {
        script_condition_free(conditionHandle);
        return -1;
    }
    Breakpoint bp;
    bp.type = type;
    bp.address = address;
    bp.condition = condition;
    bp.conditionHandle = conditionHandle;
    bp.action = action;
    bps.push_back(bp);
    rebuildBreakpointIndex();
    return static_cast<int>(bps.size() - 1);
}

bool Emulator::removeBreakpoint(unsigned slot)
{
    if (slot >= bps.size()) {
        return false;
    }
    script_condition_free(bps[slot].conditionHandle);
    bps.erase(bps.begin() + slot);
    rebuildBreakpointIndex();
    return true;
}

void Emulator::removeAllBreakpoints()
{
    for (const Breakpoint &bp : bps) {
        script_condition_free(bp.conditionHandle);
    }
    bps.clear();
    rebuildBreakpointIndex();
}

bool Emulator::setBreakpointEnabled(unsigned slot, bool enabled)
{
    if (slot >= bps.size()) {
        return false;
    }
    bps[slot].enabled = enabled;
    rebuildBreakpointIndex();
    return true;
}

int Emulator::breakpointAt(uint16_t address) const
{
    for (unsigned i = 0; i < bps.size(); i++) {
        if (bps[i].enabled && bps[i].type == Breakpoint::Type::Execute && bps[i].address == address) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Emulator::rebuildBreakpointIndex()
{
    execIndex.fill(0);
    accessIndex.fill(0);
    opcodeIndex.fill(0);
    for (const Breakpoint &bp : bps) {
        if (!bp.enabled) {
            continue;
        }
        switch (bp.type) {
        case Breakpoint::Type::Execute: execIndex[bp.address] = 1; break;
        case Breakpoint::Type::Read: accessIndex[bp.address] |= 1; break;
        case Breakpoint::Type::Write: accessIndex[bp.address] |= 2; break;
        case Breakpoint::Type::ReadWrite: accessIndex[bp.address] |= 3; break;
        case Breakpoint::Type::Opcode: opcodeIndex[bp.address & 0xFF] = 1; break;
        }
    }
}
