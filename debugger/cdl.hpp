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
// Code/data log: for every byte of PRG-ROM, whether the CPU ran it as code, read it as data, or reached it by a jump.
// Kept per ROM offset (so each bank of a bank-switched game has its own record). Saved and loaded in Mesen's CDL
// format ("CDLv2", the CRC-32 of the PRG-ROM, one flag byte per PRG-ROM byte, then one per CHR-ROM byte), so
// Mesen's debugger and disassembler tools can use the file and the other way round.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "flow.hpp"

class CodeDataLog {
public:
    // The flags are Mesen's, checked against a CDL Mesen wrote for Mike Tyson's Punch-Out!! (code 1, data 2,
    // jump target 4, subroutine entry 8; DMC sample bytes were data + 0x80). A byte can have several.
    enum Flag : uint8_t {
        Code = 0x01,          // an opcode or an operand of an instruction the CPU ran
        Data = 0x02,          // read by an instruction (not fetched as part of one)
        JumpTarget = 0x04,    // jumped or branched to (also reached by JMP (ind), or an RTS to somewhere else)
        SubEntryPoint = 0x08, // the start of a subroutine (JSR target) or an interrupt handler
        PcmData = 0x80,       // sample data read by the DMC (with Data)
    };

    bool enabled() const { return on; }
    void setEnabled(bool enable); // (sizes the log to the loaded game's PRG-ROM)
    void clear();                 // forget everything recorded (and re-size to the loaded game)
    void discard()                // another game is coming: sized to it when next needed
    {
        flags.clear();
        chrFlags.clear();
    }
    size_t size() const { return flags.size(); }

    // The CPU is about to run the instruction at PC, which was reached the way ARRIVAL says
    void onInstruction(uint16_t pc, uint8_t opcode, Arrival arrival);
    // The CPU read ADDRESS (not a write; instruction fetches are told apart from data reads by onInstruction())
    void onRead(uint16_t address);
    // The DMC read a sample byte at ADDRESS (DMA: not an instruction's read)
    void onDmcRead(uint16_t address);

    uint8_t flagsAt(int32_t offset) const;
    // By hand (cdl mark / unmark): LENGTH bytes from PRG-ROM offset FIRST become data (Code, JumpTarget and
    // SubEntryPoint cleared, so the code window shows them as bytes), or lose their Data flag. Sizes the log to the
    // game if needed (it need not be on). Returns the number of bytes changed.
    size_t markData(size_t first, size_t length, bool data);
    // What the code window shows as bytes rather than instructions: read as data and never run as code
    bool isData(int32_t offset) const
    {
        const uint8_t f = flagsAt(offset);
        return (f & Data) != 0 && (f & Code) == 0;
    }

    struct Counts {
        size_t bytes = 0, code = 0, data = 0, unknown = 0;
        size_t pcm = 0; // of the data: the DMC's samples
    };
    // Bytes of PRG-ROM between FIRST (inclusive) and LAST (exclusive): how much is code, data, still unknown
    Counts count(size_t first, size_t last) const;
    // Ranges [first, last) of 0 flags (never seen), at least MINIMUM bytes long
    std::vector<std::pair<size_t, size_t>> unseen(size_t first, size_t last, size_t minimum) const;

    // Mesen's format. load() ORs the file into what is recorded (so logs from several sessions add up); a file for
    // another ROM (the CRC-32 differs) is loaded only if FORCE. Return "" or what went wrong.
    std::string save(const std::string &file) const;
    std::string load(const std::string &file, bool force);

private:
    bool ready() const; // the log matches the loaded game
    void size_to_game();
    void markCode(uint16_t pc, unsigned length, uint8_t extra);

    bool on = false;
    std::vector<uint8_t> flags;    // one per PRG-ROM byte
    std::vector<uint8_t> chrFlags; // one per CHR-ROM byte: not recorded, but kept from a file that has them (Mesen's)
    uint16_t instructionPc = 0;
    uint8_t instructionLength = 1;
};
