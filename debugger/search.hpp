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
// Memory search ("cs" commands): find a variable by how its value or its accesses change while the game runs.
// A search tracks every byte of one memory space; each filter keeps the candidates that behaved as asked since the
// previous filter (or the start), then takes a new snapshot.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class MemorySearch {
public:
    enum class Target { Cpu, Ppu, Oam };
    enum class Op { Equal, NotEqual, Less, Greater, Same, Changed, Up, Down, Plus, Minus };

    struct Candidate {
        uint32_t address;
        uint8_t current;
        uint8_t previous; // the value the last filter compared against (at the filter before it, or the start)
        int lastWriter;   // PC of the last instruction that wrote it (CPU target), -1 if unknown
    };

    bool active() const { return isActive; }
    Target target() const { return searchTarget; }
    bool isSigned() const { return signedCompare; }
    void setSigned(bool s) { signedCompare = s; }

    void start(Target target); // snapshot every byte of the target's memory; all are candidates
    void stop();

    // Filters judge one stretch of play: from the time the game last started running after a filter (or after
    // cs start) to now. Several filters typed one after another, without the game running in between, all judge
    // the same stretch ("cs same" then "cs notwritten": unchanged AND not written in that stretch); the next
    // stretch starts when the game runs again (beforeRun()).
    size_t filterValue(Op op, int value = 0);      // value: for Equal..Greater, Plus/Minus
    size_t filterAccess(bool writes, bool wanted); // CPU target: keep (not) written / read in the stretch
    bool undo();                                   // back to the candidates (and stretch) before the last filter
    void beforeRun() // the emulator is about to run an instruction: a new stretch starts if a filter ended one
    {
        if (newStretchPending) {
            newStretchPending = false;
            snapshot();
        }
    }
    size_t count() const { return candidates.size(); }
    size_t undoDepth() const { return history.size(); }
    std::vector<Candidate> list(size_t from, size_t max) const;
    std::string describeTarget() const; // "CPU RAM $0000-$07FF, $6000-$7FFF"

    // From the emulator's memory access hook while a CPU search is active
    void onCpuAccess(uint16_t address, bool write, uint16_t pc);

private:
    uint8_t read(size_t slot) const;
    int compareValue(uint8_t v) const { return signedCompare ? static_cast<int8_t>(v) : v; }
    void snapshot();
    int slotOfCpuAddress(uint16_t address) const;

    bool isActive = false;
    bool signedCompare = false;
    Target searchTarget = Target::Cpu;
    std::vector<uint32_t> addresses;  // slot -> address in the target's space
    std::vector<uint32_t> candidates; // slots still in the running
    std::vector<uint8_t> previous;    // slot -> value at the last filter (what the next filter compares to)
    std::vector<uint8_t> compared;    // slot -> what the last filter compared against (shown as "was")
    std::vector<uint8_t> written, wasRead;
    std::vector<int32_t> lastWriter;
    size_t prgRamSize = 0;
    bool newStretchPending = false; // a filter ended the stretch: the next one starts when the game runs
    struct Saved {
        std::vector<uint32_t> candidates;
        std::vector<uint8_t> previous;
        std::vector<uint8_t> compared;
        std::vector<uint8_t> written, wasRead;
        bool newStretchPending;
    };
    std::vector<Saved> history; // for undo
};
