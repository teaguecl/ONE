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
#include "search.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <utility>
#include <string>
#include "core.hpp"

namespace {
constexpr size_t kInternalRam = 0x800;
constexpr size_t kUndoDepth = 32;
}

void MemorySearch::start(Target target)
{
    searchTarget = target;
    addresses.clear();
    prgRamSize = 0;
    switch (target) {
    case Target::Cpu: {
        for (uint32_t a = 0; a < kInternalRam; a++) {
            addresses.push_back(a);
        }
        size_t size = 0;
        if (mapper_debug_prg_ram(&size) != nullptr) {
            prgRamSize = std::min<size_t>(size, 0x2000);
            for (uint32_t a = 0; a < prgRamSize; a++) {
                addresses.push_back(0x6000 + a);
            }
        }
        break;
    }
    case Target::Ppu:
        for (uint32_t a = 0; a < 0x3000; a++) { // pattern tables (CHR) and nametables
            addresses.push_back(a);
        }
        for (uint32_t a = 0x3F00; a < 0x3F20; a++) { // palette
            addresses.push_back(a);
        }
        break;
    case Target::Oam:
        for (uint32_t a = 0; a < 0x100; a++) {
            addresses.push_back(a);
        }
        break;
    }
    candidates.resize(addresses.size());
    for (uint32_t i = 0; i < candidates.size(); i++) {
        candidates[i] = i;
    }
    previous.assign(addresses.size(), 0);
    lastWriter.assign(addresses.size(), -1);
    history.clear();
    isActive = true;
    newStretchPending = false;
    snapshot();
    compared = previous;
}

void MemorySearch::stop()
{
    isActive = false;
    addresses.clear();
    candidates.clear();
    previous.clear();
    compared.clear();
    written.clear();
    wasRead.clear();
    lastWriter.clear();
    history.clear();
}

uint8_t MemorySearch::read(size_t slot) const
{
    uint32_t a = addresses[slot];
    switch (searchTarget) {
    case Target::Cpu:
        if (a >= 0x6000) {
            size_t size = 0;
            uint8_t *ram = mapper_debug_prg_ram(&size); // the RAM itself, even while the mapper disables it
            return (ram && a - 0x6000 < size) ? ram[a - 0x6000] : 0;
        }
        return cpu_debug_read(static_cast<uint16_t>(a));
    case Target::Ppu: return ppu_debug_read(static_cast<uint16_t>(a));
    case Target::Oam: return ppu_debug_read_oam(static_cast<uint8_t>(a));
    }
    return 0;
}

void MemorySearch::snapshot()
{
    for (size_t i = 0; i < addresses.size(); i++) {
        previous[i] = read(i);
    }
    written.assign(addresses.size(), 0);
    wasRead.assign(addresses.size(), 0);
}

size_t MemorySearch::filterValue(Op op, int value)
{
    history.push_back({candidates, previous, compared, written, wasRead, newStretchPending});
    if (history.size() > kUndoDepth) {
        history.erase(history.begin());
    }
    std::vector<uint32_t> kept;
    for (uint32_t slot : candidates) {
        uint8_t now = read(slot), before = previous[slot];
        int cur = compareValue(now), prev = compareValue(before);
        int target = compareValue(static_cast<uint8_t>(value));
        bool keep = false;
        switch (op) {
        case Op::Equal: keep = (now == static_cast<uint8_t>(value)); break;
        case Op::NotEqual: keep = (now != static_cast<uint8_t>(value)); break;
        case Op::Less: keep = (cur < target); break;
        case Op::Greater: keep = (cur > target); break;
        case Op::Same: keep = (now == before); break;
        case Op::Changed: keep = (now != before); break;
        case Op::Up: keep = (cur > prev); break;
        case Op::Down: keep = (cur < prev); break;
        case Op::Plus: keep = (static_cast<uint8_t>(now - before) == static_cast<uint8_t>(value)); break;
        case Op::Minus: keep = (static_cast<uint8_t>(before - now) == static_cast<uint8_t>(value)); break;
        }
        if (keep) {
            kept.push_back(slot);
        }
    }
    candidates.swap(kept);
    compared = previous;
    newStretchPending = true; // (not snapshot(): more filters on this stretch may follow)
    return candidates.size();
}

size_t MemorySearch::filterAccess(bool writes, bool wanted)
{
    history.push_back({candidates, previous, compared, written, wasRead, newStretchPending});
    if (history.size() > kUndoDepth) {
        history.erase(history.begin());
    }
    std::vector<uint32_t> kept;
    const std::vector<uint8_t> &flags = writes ? written : wasRead;
    for (uint32_t slot : candidates) {
        if ((flags[slot] != 0) == wanted) {
            kept.push_back(slot);
        }
    }
    candidates.swap(kept);
    compared = previous;
    newStretchPending = true; // (not snapshot(): more filters on this stretch may follow)
    return candidates.size();
}

bool MemorySearch::undo()
{
    if (history.empty()) {
        return false;
    }
    Saved &saved = history.back();
    candidates = std::move(saved.candidates);
    previous = std::move(saved.previous);
    compared = std::move(saved.compared);
    written = std::move(saved.written);
    wasRead = std::move(saved.wasRead);
    newStretchPending = saved.newStretchPending;
    history.pop_back();
    return true;
}

std::vector<MemorySearch::Candidate> MemorySearch::list(size_t from, size_t max) const
{
    std::vector<Candidate> out;
    for (size_t i = from; i < candidates.size() && out.size() < max; i++) {
        uint32_t slot = candidates[i];
        out.push_back({addresses[slot], read(slot), compared[slot], lastWriter[slot]});
    }
    return out;
}

std::string MemorySearch::describeTarget() const
{
    switch (searchTarget) {
    case Target::Cpu: return prgRamSize ? "CPU RAM $0000-$07FF and cartridge RAM $6000-$7FFF" : "CPU RAM $0000-$07FF";
    case Target::Ppu: return "PPU memory $0000-$2FFF and palette $3F00-$3F1F";
    case Target::Oam: return "OAM (sprite memory) $00-$FF";
    }
    return "";
}

int MemorySearch::slotOfCpuAddress(uint16_t address) const
{
    if (address < 0x2000) {
        return address & 0x7FF; // $0800-$1FFF mirror internal RAM
    }
    if (address >= 0x6000 && address < 0x6000 + prgRamSize) {
        return static_cast<int>(kInternalRam + (address - 0x6000));
    }
    return -1;
}

void MemorySearch::onCpuAccess(uint16_t address, bool write, uint16_t pc)
{
    int slot = slotOfCpuAddress(address);
    if (slot < 0) {
        return;
    }
    if (write) {
        written[slot] = 1;
        lastWriter[slot] = pc;
    } else {
        wasRead[slot] = 1;
    }
}
