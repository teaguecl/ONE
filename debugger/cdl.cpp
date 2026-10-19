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
#include "cdl.hpp"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>
#include <utility>
#include <string>
#include <ios>
#include <iterator>
#include "core.hpp"
#include "debugger/flow.hpp"
#include "disasm.hpp"

extern "C" {
#include "crc32.h"
}

namespace {
constexpr char kHeader[] = "CDLv2";
constexpr size_t kHeaderSize = 5;

size_t prgSize()
{
    size_t size = 0;
    mapper_debug_prg_rom(&size);
    return size;
}

size_t chrSize()
{
    size_t size = 0;
    mapper_debug_chr_rom(&size);
    return size;
}

uint32_t prgCrc()
{
    size_t size = 0;
    const uint8_t *data = mapper_debug_prg_rom(&size);
    if (data == nullptr) {
        return 0;
    }
    return crc32_update(0xFFFFFFFFu, data, size) ^ 0xFFFFFFFFu;
}
} // namespace

bool CodeDataLog::ready() const
{
    return !flags.empty() && flags.size() == prgSize();
}

void CodeDataLog::size_to_game()
{
    flags.assign(prgSize(), 0);
    chrFlags.assign(chrSize(), 0);
}

void CodeDataLog::setEnabled(bool enable)
{
    on = enable;
    if (on && !ready()) {
        size_to_game();
    }
}

void CodeDataLog::clear()
{
    size_to_game();
}

uint8_t CodeDataLog::flagsAt(int32_t offset) const
{
    return offset >= 0 && static_cast<size_t>(offset) < flags.size() ? flags[static_cast<size_t>(offset)] : 0;
}

size_t CodeDataLog::markData(size_t first, size_t length, bool data)
{
    if (!ready()) {
        size_to_game();
    }
    size_t changed = 0;
    for (size_t i = first; i < first + length && i < flags.size(); i++) {
        const uint8_t before = flags[i];
        flags[i] = data ? static_cast<uint8_t>((before & ~(Code | JumpTarget | SubEntryPoint)) | Data)
                        : static_cast<uint8_t>(before & ~(Data | PcmData));
        changed += flags[i] != before;
    }
    return changed;
}

void CodeDataLog::markCode(uint16_t pc, unsigned length, uint8_t extra)
{
    for (unsigned i = 0; i < length; i++) {
        const uint16_t address = static_cast<uint16_t>(pc + i);
        if (address < 0x8000) {
            return; // code in RAM is not in the ROM
        }
        const int32_t offset = mapper_debug_prg_offset(address);
        if (offset >= 0 && static_cast<size_t>(offset) < flags.size()) {
            flags[static_cast<size_t>(offset)] |= Code | (i == 0 ? extra : 0);
        }
    }
}

void CodeDataLog::onInstruction(uint16_t pc, uint8_t opcode, Arrival arrival)
{
    if (!on) {
        return;
    }
    if (!ready()) {
        size_to_game();
    }
    instructionPc = pc;
    instructionLength = Disassembler::size(opcode);
    uint8_t extra = 0;
    switch (arrival) {
    case Arrival::Call:
    case Arrival::Interrupt:
    case Arrival::Reset: extra = SubEntryPoint; break;
    case Arrival::Jump: extra = JumpTarget; break;
    case Arrival::JumpIndirect:
    case Arrival::Dispatch: extra = JumpTarget; break;
    case Arrival::Sequential:
    case Arrival::Return: break;
    }
    markCode(pc, instructionLength, extra);
}

void CodeDataLog::onRead(uint16_t address)
{
    if (!on || address < 0x8000 || flags.empty()) {
        return;
    }
    if (static_cast<uint16_t>(address - instructionPc) < instructionLength) {
        return; // the instruction's own bytes
    }
    const int32_t offset = mapper_debug_prg_offset(address);
    if (offset < 0 || static_cast<size_t>(offset) >= flags.size()) {
        return;
    }
    flags[static_cast<size_t>(offset)] |= Data;
}

void CodeDataLog::onDmcRead(uint16_t address)
{
    if (!on || address < 0x8000 || flags.empty()) {
        return;
    }
    const int32_t offset = mapper_debug_prg_offset(address);
    if (offset >= 0 && static_cast<size_t>(offset) < flags.size()) {
        flags[static_cast<size_t>(offset)] |= Data | PcmData;
    }
}

CodeDataLog::Counts CodeDataLog::count(size_t first, size_t last) const
{
    Counts counts;
    if (last > flags.size()) {
        last = flags.size();
    }
    for (size_t i = first; i < last; i++) {
        const uint8_t f = flags[i];
        counts.bytes++;
        if (f == 0) {
            counts.unknown++;
        } else if (f & Code) {
            counts.code++;
        } else if (f & (Data | PcmData)) {
            counts.data++;
            if (f & PcmData) {
                counts.pcm++;
            }
        } else {
            counts.unknown++; // (a jump target that was never run: no better than unknown)
        }
    }
    return counts;
}

std::vector<std::pair<size_t, size_t>> CodeDataLog::unseen(size_t first, size_t last, size_t minimum) const
{
    std::vector<std::pair<size_t, size_t>> ranges;
    if (last > flags.size()) {
        last = flags.size();
    }
    size_t start = first;
    bool inRange = false;
    for (size_t i = first; i <= last; i++) {
        const bool zero = i < last && flags[i] == 0;
        if (zero && !inRange) {
            start = i;
            inRange = true;
        } else if (!zero && inRange) {
            inRange = false;
            if (i - start >= minimum) {
                ranges.emplace_back(start, i);
            }
        }
    }
    return ranges;
}

std::string CodeDataLog::save(const std::string &file) const
{
    if (!ready()) {
        return "nothing recorded (cdl on, then play)";
    }
    std::ofstream out(file, std::ios::binary);
    if (!out) {
        return "cannot write '" + file + "'";
    }
    out.write(kHeader, kHeaderSize);
    const uint32_t crc = prgCrc();
    const char crcBytes[4] = {static_cast<char>(crc), static_cast<char>(crc >> 8), static_cast<char>(crc >> 16),
                              static_cast<char>(crc >> 24)};
    out.write(crcBytes, 4);
    out.write(reinterpret_cast<const char *>(flags.data()), static_cast<std::streamsize>(flags.size()));
    std::vector<uint8_t> chr = chrFlags; // (not recorded: only what a loaded file had)
    chr.resize(chrSize(), 0);
    out.write(reinterpret_cast<const char *>(chr.data()), static_cast<std::streamsize>(chr.size()));
    return out ? "" : "cannot write '" + file + "'";
}

std::string CodeDataLog::load(const std::string &file, bool force)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return "cannot read '" + file + "'";
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t prg = prgSize();
    if (prg == 0) {
        return "no game loaded";
    }
    if (data.size() < kHeaderSize + 4 || std::string(data.begin(), data.begin() + kHeaderSize) != kHeader) {
        return "'" + file + "' is not a Mesen CDL file (CDLv2)";
    }
    const uint32_t crc = data[5] | (data[6] << 8) | (data[7] << 16) | (static_cast<uint32_t>(data[8]) << 24);
    if (data.size() < kHeaderSize + 4 + prg) {
        return "'" + file + "' has too little data for this game's PRG-ROM";
    }
    if (crc != prgCrc() && !force) {
        char text[96];
        std::snprintf(text, sizeof(text),
                      "the CRC-32 in '%s' (%08X) is not this game's PRG-ROM's (%08X): cdl load FILE force",
                      file.c_str(), crc, prgCrc());
        return text;
    }
    if (!ready()) {
        size_to_game();
    }
    for (size_t i = 0; i < prg; i++) {
        flags[i] |= data[kHeaderSize + 4 + i];
    }
    const size_t chrStart = kHeaderSize + 4 + prg;
    if (chrFlags.size() != chrSize()) {
        chrFlags.assign(chrSize(), 0);
    }
    for (size_t i = 0; i < chrFlags.size() && chrStart + i < data.size(); i++) {
        chrFlags[i] |= data[chrStart + i];
    }
    return "";
}
