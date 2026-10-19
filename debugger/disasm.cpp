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
#include "disasm.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "core.hpp"
#include "emulator.hpp"
#include "symbols.hpp"

namespace {
std::string hex(unsigned value, int digits)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%0*X", digits, value);
    return buf;
}
} // namespace

uint8_t Disassembler::size(uint8_t opcode)
{
    return addressing_mode_sizes[instruction_info[opcode].mode];
}

std::string Disassembler::addressColumn(uint16_t address)
{
    char text[16];
    int bank = 0, banks = 0;
    if (mapper_debug_prg_bank(address, &bank, &banks)) {
        std::snprintf(text, sizeof(text), "%02X:%04X", bank, address);
    } else if (mapper_debug_prg_bank(0x8000, &bank, &banks)) {
        std::snprintf(text, sizeof(text), "   %04X", address); // (lined up with the banked ones)
    } else {
        std::snprintf(text, sizeof(text), "%04X", address);
    }
    return text;
}

std::string Disassembler::memoryBankNote(uint16_t address)
{
    int bank = 0, banks = 0;
    if (!mapper_debug_prg_bank(address, &bank, &banks)) {
        return "";
    }
    char text[40];
    std::snprintf(text, sizeof(text), "   $%04X: PRG bank %d", address, bank);
    return text;
}

std::string Disassembler::bankText(uint16_t address)
{
    int bank = 0, banks = 0;
    if (!mapper_debug_prg_bank(address, &bank, &banks)) {
        return "";
    }
    char text[32];
    std::snprintf(text, sizeof(text), "PRG bank %d of %d", bank, banks);
    return text;
}

std::string Disassembler::addressText(uint16_t address, bool zeroPage) const
{
    if (const std::string *label = symbols.find(address)) {
        return *label;
    }
    return "$" + hex(address, zeroPage ? 2 : 4);
}

uint8_t Disassembler::read(uint16_t address) const
{
    if (bank != nullptr && address >= 0x8000) {
        return bank[address % bankSize];
    }
    return cpu_debug_read(address);
}

DisasmLine Disassembler::decode(uint16_t address, bool withState) const
{
    DisasmLine line;
    withState = withState && bank == nullptr;
    const uint8_t opcode = read(address);
    const instruction_data &info = instruction_info[opcode];
    const uint8_t lo = read(static_cast<uint16_t>(address + 1));
    const uint8_t hi = read(static_cast<uint16_t>(address + 2));
    const uint16_t abs = static_cast<uint16_t>(lo | (hi << 8));
    const uint8_t x = cpu_debug_get_reg_x(), y = cpu_debug_get_reg_y();

    line.address = address;
    line.size = addressing_mode_sizes[info.mode];
    line.mnemonic = mnemonic_strings[info.type];
    line.documented = info.is_documented;
    line.bytes = hex(opcode, 2);
    for (int i = 1; i < line.size; i++) {
        line.bytes += " " + hex(i == 1 ? lo : hi, 2);
    }

    int effective = -1; // address the instruction reads/writes (for the comment), -1 = none
    switch (info.mode) {
    case IMPLIED: break;
    case ACCUMULATOR: line.operand = "A"; break;
    case IMMEDIATE: line.operand = "#$" + hex(lo, 2); break;
    case ZERO_PAGE:
        line.operand = addressText(lo, true);
        effective = lo;
        break;
    case ZERO_PAGE_X:
        line.operand = addressText(lo, true) + ",X";
        effective = (lo + x) & 0xFF;
        break;
    case ZERO_PAGE_Y:
        line.operand = addressText(lo, true) + ",Y";
        effective = (lo + y) & 0xFF;
        break;
    case ABSOLUTE:
        line.operand = addressText(abs, false);
        if (info.type != JMP && info.type != JSR) {
            effective = abs;
        }
        break;
    case ABSOLUTE_X:
        line.operand = addressText(abs, false) + ",X";
        effective = (abs + x) & 0xFFFF;
        break;
    case ABSOLUTE_Y:
        line.operand = addressText(abs, false) + ",Y";
        effective = (abs + y) & 0xFFFF;
        break;
    case INDIRECT: {
        line.operand = "(" + addressText(abs, false) + ")";
        // JMP ($xxFF) wraps within the page on the 6502
        uint16_t target = static_cast<uint16_t>(
            cpu_debug_read(abs) | (cpu_debug_read(static_cast<uint16_t>((abs & 0xFF00) | ((abs + 1) & 0xFF))) << 8));
        if (withState) {
            line.comment = "-> " + addressText(target, false);
        }
        break;
    }
    case INDIRECT_X: {
        line.operand = "(" + addressText(lo, true) + ",X)";
        uint8_t zp = static_cast<uint8_t>(lo + x);
        effective = cpu_debug_read(zp) | (cpu_debug_read(static_cast<uint8_t>(zp + 1)) << 8);
        break;
    }
    case INDIRECT_Y: {
        line.operand = "(" + addressText(lo, true) + "),Y";
        uint16_t base = static_cast<uint16_t>(cpu_debug_read(lo) | (cpu_debug_read(static_cast<uint8_t>(lo + 1)) << 8));
        effective = (base + y) & 0xFFFF;
        break;
    }
    case RELATIVE: {
        uint16_t target = static_cast<uint16_t>(address + 2 + static_cast<int8_t>(lo));
        line.operand = addressText(target, false);
        if (withState) {
            // branch opcodes: bits 7-6 select the flag (N V C Z), bit 5 the value that takes the branch
            static const uint8_t flagMask[4] = {0x80, 0x40, 0x01, 0x02};
            bool flagSet = (cpu_debug_get_reg_p() & flagMask[opcode >> 6]) != 0;
            bool taken = (flagSet == ((opcode & 0x20) != 0));
            line.comment = taken ? "(jump)" : "(no jump)";
        }
        break;
    }
    default: break;
    }

    if (withState && effective >= 0) {
        line.comment = addressText(static_cast<uint16_t>(effective), false);
        if (effective >= 0x2000 && effective < 0x4020) {
            line.comment += " (I/O)"; // reading registers here has side effects, so don't show a value
        } else {
            line.comment += " = $" + hex(cpu_debug_read(static_cast<uint16_t>(effective)), 2);
        }
    }
    return line;
}

DisasmLine Disassembler::dataByte(uint16_t address)
{
    DisasmLine line;
    line.address = address;
    line.size = 1;
    line.bytes = hex(cpu_debug_read(address), 2);
    line.mnemonic = ".db";
    line.operand = "$" + line.bytes;
    return line;
}

bool Disassembler::isData(uint16_t address) const
{
    if (address < 0x8000 || bank != nullptr) {
        return false; // not ROM, or a bank shown without being mapped in
    }
    return Emulator::instance().codeLog().isData(mapper_debug_prg_offset(address));
}

// A data row starts at a multiple of 8 and at anything that must stay visible by itself
namespace {
bool dataRowBreak(const Disassembler &d, const Symbols &symbols, uint16_t address)
{
    return (address & 7) == 0 || !d.isData(address) || symbols.find(address) != nullptr ||
           symbols.commentAt(address) != nullptr || Emulator::instance().breakpointAt(address) >= 0 ||
           address == cpu_debug_get_pc();
}
} // namespace

DisasmLine Disassembler::dataRow(uint16_t address) const
{
    DisasmLine line;
    line.address = address;
    unsigned size = 1;
    while (size < 8 && address + size <= 0xFFFF &&
           !dataRowBreak(*this, symbols, static_cast<uint16_t>(address + size))) {
        size++;
    }
    line.size = static_cast<uint8_t>(size);
    line.mnemonic = ".db";
    for (unsigned i = 0; i < size; i++) {
        line.operand += (i ? " " : "") + hex(cpu_debug_read(static_cast<uint16_t>(address + i)), 2);
    }
    return line;
}

uint16_t Disassembler::dataRowStart(uint16_t address) const
{
    // walk the rows forward from the multiple of 8 at or below the byte
    for (unsigned start = address & ~7u; start <= address;) {
        if (!isData(static_cast<uint16_t>(start))) {
            start++;
            continue;
        }
        const unsigned next = start + dataRow(static_cast<uint16_t>(start)).size;
        if (next > address) {
            return static_cast<uint16_t>(start);
        }
        start = next;
    }
    return address;
}

bool Disassembler::knownStart(uint16_t address)
{
    return address == cpu_debug_get_pc() || Emulator::instance().executedAt(address);
}

// The candidates for the instruction that ends at target: the 1-3 byte instructions whose last byte is just before
// target, with how likely each is to be real code (see previous()). An instruction cannot cover the start of one the
// CPU executed (or PC).
std::vector<Disassembler::Candidate> Disassembler::candidates(uint16_t target) const
{
    const Emulator &emu = Emulator::instance();
    auto opcodeAt = [](int address) { return cpu_debug_read(static_cast<uint16_t>(address)); };
    auto decodable = [](uint8_t opcode) { return instruction_info[opcode].type != ILL; };
    auto coversStart = [](int address, int length) {
        for (int i = 1; i < length; i++) {
            if (knownStart(static_cast<uint16_t>(address + i))) {
                return true;
            }
        }
        return false;
    };
    // How many ways of decoding forward from the bytes before CANDIDATE land exactly on it: each start point in
    // the 48 bytes before it whose instructions run into CANDIDATE counts 2 if they are all documented opcodes, 1
    // if some are unofficial (none undecodable). Code reached from many start points is likely real code.
    auto agreement = [&](int candidate) {
        int votes = 0;
        for (int start = std::max(0, candidate - 48); start < candidate; start++) {
            int a = start;
            bool documented = true;
            bool decodes = true;
            while (a < candidate) {
                uint8_t op = opcodeAt(a);
                if (!decodable(op) || coversStart(a, size(op))) {
                    decodes = false;
                    break;
                }
                documented = documented && instruction_info[op].is_documented;
                a += size(op);
            }
            if (decodes && a == candidate) {
                votes += documented ? 2 : 1;
            }
        }
        return votes;
    };
    std::vector<Candidate> found;
    for (int length = 1; length <= 3; length++) {
        int candidate = target - length;
        if (candidate < 0) {
            break;
        }
        uint8_t op = opcodeAt(candidate);
        if (!decodable(op) || size(op) != length || coversStart(candidate, length)) {
            continue;
        }
        int kind = emu.executedAt(static_cast<uint16_t>(candidate)) ? 2 : instruction_info[op].is_documented ? 1 : 0;
        found.push_back({static_cast<uint16_t>(candidate), kind, agreement(candidate)});
    }
    return found;
}

// The best chain of up to 'lines' instructions that runs exactly into target, as the addresses from target back.
// Every chain lands on target when decoded forward, so its last address is a safe place to start the code window.
// Chains are compared step by step from target: an instruction the CPU executed beats a documented opcode, which
// beats an unofficial one, so an unofficial opcode is only used when no documented one fits there (or the CPU ran
// it). Equal so far, the longer chain wins, then the step more decodings from earlier bytes agree on.
std::vector<uint16_t> Disassembler::chain(uint16_t target, int lines) const
{
    struct Best {
        std::vector<int> kinds;     // per step from target: 2 executed, 1 documented, 0 unofficial
        std::vector<uint16_t> path; // the addresses, nearest target first
        long agreement = 0;         // of the first step
    };
    std::map<std::pair<uint16_t, int>, Best> memo;
    std::map<uint16_t, std::vector<Candidate>> found; // candidates() once per address
    auto candidatesAt = [&](uint16_t address) -> const std::vector<Candidate> & {
        auto it = found.find(address);
        return it != found.end() ? it->second : found[address] = candidates(address);
    };
    auto better = [](const Best &a, const Best &b) {
        if (a.kinds != b.kinds) {
            // lexicographic, and a chain that continues beats one that ends there
            return std::lexicographical_compare(b.kinds.begin(), b.kinds.end(), a.kinds.begin(), a.kinds.end());
        }
        return a.agreement > b.agreement;
    };
    std::function<const Best &(uint16_t, int)> best = [&](uint16_t address, int depth) -> const Best & {
        auto key = std::make_pair(address, depth);
        auto it = memo.find(key);
        if (it != memo.end()) {
            return it->second;
        }
        Best result;
        if (depth > 0) {
            for (const Candidate &c : candidatesAt(address)) {
                const Best &rest = best(c.address, depth - 1);
                Best option;
                option.kinds.push_back(c.kind);
                option.kinds.insert(option.kinds.end(), rest.kinds.begin(), rest.kinds.end());
                option.path.push_back(c.address);
                option.path.insert(option.path.end(), rest.path.begin(), rest.path.end());
                option.agreement = c.agreement;
                if (result.path.empty() || better(option, result)) {
                    result = std::move(option);
                }
            }
        }
        return memo[key] = std::move(result);
    };
    return best(target, lines).path;
}

uint16_t Disassembler::backtrack(uint16_t target, int lines) const
{
    std::vector<uint16_t> path = chain(target, lines);
    return path.empty() ? target : path.back();
}

uint16_t Disassembler::previous(uint16_t target) const
{
    // Look several instructions back so that the step does not lead into a dead end (bytes no instruction ends at)
    std::vector<uint16_t> path = chain(target, 8);
    return path.empty() ? static_cast<uint16_t>(target - 1) : path.front();
}
