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
#include "flow.hpp"
#include "core.hpp"
#include "disasm.hpp"
#include <cstdint>

namespace {
bool isBranch(uint8_t opcode)
{
    return (opcode & 0x1F) == 0x10; // BPL BMI BVC BVS BCC BCS BNE BEQ
}

bool branchTaken(uint8_t opcode, uint8_t p)
{
    static const uint8_t flags[4] = {0x80, 0x40, 0x01, 0x02}; // N V C Z
    const bool set = (p & flags[opcode >> 6]) != 0;
    return set == (((opcode >> 5) & 1) != 0);
}

uint8_t stackByte(unsigned offset)
{
    return cpu_debug_read(static_cast<uint16_t>(0x0100 | (offset & 0xFF)));
}
} // namespace

void FlowTracker::reset()
{
    havePrevious = false;
    lastArrival = Arrival::Sequential;
    stack.clear();
    recent.clear();
    if (listener != nullptr) {
        listener->onReset();
    }
}

void FlowTracker::push(CallFrame::Kind kind, uint16_t callSite, uint16_t target, uint16_t ret, uint8_t sp)
{
    if (stack.size() >= kMaxFrames) {
        stack.erase(stack.begin()); // the oldest: a runaway recursion must not grow without limit
        if (listener != nullptr) {
            listener->onDrop();
        }
    }
    CallFrame frame;
    frame.kind = kind;
    frame.callSite = callSite;
    frame.target = target;
    frame.returnAddress = ret;
    frame.entrySp = sp;
    frame.callSiteOffset = mapper_debug_prg_offset(callSite);
    frame.targetOffset = mapper_debug_prg_offset(target);
    stack.push_back(frame);
    if (listener != nullptr) {
        listener->onEnter(frame, nowCycle);
    }
}

// Where the instruction at PC (about to run, with the CPU in its current state) sends the CPU next
uint16_t FlowTracker::predictNext(uint16_t pc, uint8_t opcode, uint8_t sp) const
{
    const uint16_t next = static_cast<uint16_t>(pc + Disassembler::size(opcode));
    switch (opcode) {
    case 0x20:
    case 0x4C: return cpu_debug_read_word(static_cast<uint16_t>(pc + 1));
    case 0x6C: {
        const uint16_t pointer = cpu_debug_read_word(static_cast<uint16_t>(pc + 1));
        const uint8_t low = cpu_debug_read(pointer);
        const uint8_t high = cpu_debug_read(static_cast<uint16_t>((pointer & 0xFF00) | ((pointer + 1) & 0xFF)));
        return static_cast<uint16_t>(low | (high << 8)); // (the 6502 does not carry into the high byte)
    }
    case 0x60: // RTS: the return address on the stack, plus one
        return static_cast<uint16_t>((stackByte(sp + 1u) | (stackByte(sp + 2u) << 8)) + 1);
    case 0x40: // RTI: P is on top, then the address
        return static_cast<uint16_t>(stackByte(sp + 2u) | (stackByte(sp + 3u) << 8));
    case 0x00: return cpu_debug_read_word(0xFFFE);
    default:
        if (isBranch(opcode) && branchTaken(opcode, cpu_debug_get_reg_p())) {
            return static_cast<uint16_t>(next + static_cast<int8_t>(cpu_debug_read(static_cast<uint16_t>(pc + 1))));
        }
        return next;
    }
}

void FlowTracker::onBoundary(uint16_t pc)
{
    const uint64_t cycle = cpu_debug_get_cycle();
    nowCycle = cycle;
    const uint8_t sp = cpu_debug_get_sp();
    const uint8_t opcode = cpu_debug_read(pc);
    lastArrival = Arrival::Sequential;

    if (havePrevious && cycle < prevCycle) { // time went backwards: a state was loaded
        reset();
    }
    if (havePrevious) {
        const uint16_t effective = prevPredicted; // where the previous instruction sent the CPU
        bool interrupt = false;
        if (pc != effective) {
            const uint16_t resetVector = cpu_debug_read_word(0xFFFC);
            const uint16_t nmiVector = cpu_debug_read_word(0xFFFA);
            const uint16_t irqVector = cpu_debug_read_word(0xFFFE);
            if (pc == resetVector) {
                stack.clear();
                if (listener != nullptr) {
                    listener->onReset();
                }
                push(CallFrame::Kind::Reset, effective, pc, effective, sp);
                lastArrival = Arrival::Reset;
            } else if (pc == nmiVector || pc == irqVector) {
                interrupt = true;
            } else {
                reset(); // the PC was changed by the debugger, or the flow is not understood: start again
                havePrevious = false;
            }
        }
        if (havePrevious && lastArrival != Arrival::Reset) {
            const uint16_t next = static_cast<uint16_t>(prevPc + prevSize);
            const uint8_t spAfter = static_cast<uint8_t>(interrupt ? sp + 3 : sp); // (the interrupt pushed 3 bytes)
            auto note = [&](Arrival how) {
                lastArrival = how;
                Transfer t;
                t.from = prevPc;
                t.to = effective;
                t.opcode = prevOpcode;
                t.fromOffset = mapper_debug_prg_offset(prevPc);
                t.toOffset = mapper_debug_prg_offset(effective);
                recent.push_back(t);
                if (recent.size() > kMaxTransfers) {
                    recent.pop_front();
                }
            };
            // Drop the frames whose stack has been given back (RTS/RTI, or code that discarded return addresses)
            auto unwind = [&](CallFrame *popped) {
                bool any = false;
                while (!stack.empty() && stack.back().entrySp < spAfter) {
                    if (popped != nullptr) {
                        *popped = stack.back();
                    }
                    any = true;
                    if (listener != nullptr) {
                        listener->onLeave(stack.back(), cycle);
                    }
                    stack.pop_back();
                }
                return any;
            };
            switch (prevOpcode) {
            case 0x20: // JSR
                push(CallFrame::Kind::Call, prevPc, effective, next, spAfter);
                lastArrival = Arrival::Call;
                break;
            case 0x4C: // JMP abs
                if (effective != next) {
                    note(Arrival::Jump);
                }
                break;
            case 0x6C: note(Arrival::JumpIndirect); break;
            case 0x60:   // RTS
            case 0x40: { // RTI
                const bool hadFrames = !stack.empty();
                CallFrame popped;
                if (unwind(&popped) && effective == popped.returnAddress) {
                    lastArrival = Arrival::Return;
                } else if (hadFrames) {
                    note(Arrival::Dispatch); // the return address was not the caller's: a table of addresses
                } else {
                    lastArrival = Arrival::Return; // nothing recorded (tracking began in the middle of a call)
                }
                break;
            }
            case 0x00: // BRK
                push(CallFrame::Kind::Brk, prevPc, effective, static_cast<uint16_t>(prevPc + 2), spAfter);
                lastArrival = Arrival::Interrupt;
                break;
            default:
                if (isBranch(prevOpcode) && effective != next) {
                    note(Arrival::Jump);
                }
                unwind(nullptr);
                break;
            }
            if (interrupt) {
                unwind(nullptr);
                push(pc == cpu_debug_read_word(0xFFFA) ? CallFrame::Kind::Nmi : CallFrame::Kind::Irq, effective, pc,
                     effective, sp);
                lastArrival = Arrival::Interrupt;
            }
        }
    }
    havePrevious = true;
    prevPc = pc;
    prevOpcode = opcode;
    prevSize = Disassembler::size(opcode);
    prevPredicted = predictNext(pc, opcode, sp);
    prevCycle = cycle;
}

const Transfer *FlowTracker::reachedBy(uint16_t pc) const
{
    if (!recent.empty() && recent.back().to == pc && lastArrival != Arrival::Sequential &&
        lastArrival != Arrival::Call && lastArrival != Arrival::Return) {
        return &recent.back();
    }
    return nullptr;
}
