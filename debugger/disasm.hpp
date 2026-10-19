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
// 6502 disassembler for the code window (uses the opcode tables shared with the trace, see debugger_priv.h).
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "symbols.hpp"

struct DisasmLine {
    uint16_t address = 0;
    uint8_t size = 1;
    std::string bytes;    // "4C F5 C5"
    std::string mnemonic; // "JMP"
    std::string operand;  // "$C5F5" or a label
    std::string comment;  // effective address / value / branch decision (current instruction only)
    bool documented = true;
};

class Disassembler {
public:
    explicit Disassembler(const Symbols &symbols) : symbols(symbols) {}

    // Decode the instruction at address. withState: add what the CPU state says about it (effective address and
    // value, branch taken or not) -- only meaningful for the instruction at PC.
    DisasmLine decode(uint16_t address, bool withState) const;
    // Decode $8000-$FFFF from one PRG-ROM bank (DATA, SIZE bytes: the mapper's bank size) instead of what the CPU
    // sees there now (the remote channel's disassemble of a bank that is not mapped in); withState is then ignored
    void useBank(const uint8_t *data, uint32_t size)
    {
        bank = data;
        bankSize = size;
    }
    static uint8_t size(uint8_t opcode);
    // The code window's address column: "05:BAB7" (PRG bank 5) for ROM in a game whose mapper banks PRG, other
    // addresses there padded to the same width ("   0300"); "BAB7" when the game has no PRG banking
    static std::string addressColumn(uint16_t address);
    // "PRG bank 5 of 8" for ROM in a bank-switched game, else ""
    static std::string bankText(uint16_t address);
    // For the CPU memory window's title: "   $BAB0: PRG bank 5" when ADDRESS (the top row) is banked ROM, else ""
    static std::string memoryBankNote(uint16_t address);
    static bool isCall(uint8_t opcode) { return opcode == 0x20; }                     // JSR
    static bool isReturn(uint8_t opcode) { return opcode == 0x60 || opcode == 0x40; } // RTS, RTI

    // Where to start showing code so that up to 'lines' instructions come before target and decoding forward lands
    // exactly on it (see chain())
    uint16_t backtrack(uint16_t target, int lines) const;
    // The instruction just before target: of the 1-3 byte instructions that end at target, the one the CPU has
    // executed (Emulator::executedAt()), else a documented opcode, else an unofficial one; among equals, the one that
    // continues furthest back, then the one most decodings from earlier addresses agree on.
    // target - 1 if no instruction ends at target.
    uint16_t previous(uint16_t target) const;
    // A one-byte data row (".db $FF"), for bytes that cannot be an instruction without covering a known one
    static DisasmLine dataByte(uint16_t address);
    // Data rows for the code window: bytes the code/data log says were read as data and never run (or that cdl mark
    // marked) show as ".db" rows of up to 8 bytes in the memory window's style ("01 30 01 70"), not as instructions.
    // A row ends at a multiple of 8, a label, a comment, a breakpoint, PC, or a byte that is not data.
    bool isData(uint16_t address) const;
    DisasmLine dataRow(uint16_t address) const;    // the row that starts at address (isData(address))
    uint16_t dataRowStart(uint16_t address) const; // where the row that holds this data byte starts
    // PC, or the start of an instruction the CPU executed: code shown must start an instruction there
    static bool knownStart(uint16_t address);

private:
    struct Candidate {
        uint16_t address;
        int kind;      // 2 executed, 1 documented opcode, 0 unofficial
        int agreement; // decodings from earlier bytes that land on it
    };
    std::vector<Candidate> candidates(uint16_t target) const;
    std::vector<uint16_t> chain(uint16_t target, int lines) const;
    std::string addressText(uint16_t address, bool zeroPage) const; // label or $hex
    uint8_t read(uint16_t address) const;                           // the CPU's view, or the bank (useBank())
    const Symbols &symbols;
    const uint8_t *bank = nullptr;
    uint32_t bankSize = 0;
};
