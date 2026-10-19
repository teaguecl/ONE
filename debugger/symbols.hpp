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
// Labels, stored the way Mesen's .mlb files do: by memory type and offset rather than by CPU address, so that a
// label in a switchable PRG-ROM bank names that bank's code only (a CPU address in $8000-$FFFF can hold any bank).
// Read from Mesen .mlb, ld65 debug info (--dbgfile; also the ';' comments of the source lines it lists) and ld65 -Ln / VICE label files; saved as .mlb.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>

class Symbols {
public:
    enum class Kind : uint8_t {
        PrgRom,      // .mlb P: offset into PRG-ROM (iNES header excluded)
        InternalRam, // .mlb R: $0000-$07FF
        CartRam,     // .mlb S (battery-backed) / W: cartridge RAM at $6000, offset from $6000
        Cpu,         // .mlb G: a CPU address (registers; labels from files that only know CPU addresses)
    };
    struct Location {
        Kind kind;
        uint32_t offset;
        bool operator<(const Location &o) const { return kind != o.kind ? kind < o.kind : offset < o.offset; }
        bool operator==(const Location &o) const { return kind == o.kind && offset == o.offset; }
    };

    // What the CPU sees at an address right now (through the mapper's current banks), and back again:
    // where the CPU sees a location right now (nullopt: a PRG-ROM bank that is not mapped in).
    static Location locate(uint16_t address);
    static std::optional<uint16_t> visibleAt(const Location &location);
    static std::string describe(const Location &location); // "PRG-ROM $1C010", "RAM $0075", ...

    void add(uint16_t address, const std::string &name); // at what the CPU sees there now (the n command)
    void add(const Location &location, const std::string &name, const std::string &comment = std::string(),
             uint32_t length = 1);
    bool remove(uint16_t address); // the label's name (a comment there stays)
    void clear();

    // Comments (Mesen .mlb keeps them with the labels; an address can have a comment without a label). The comment
    // for a CPU address is looked up like find(); setComment() puts it on what is mapped there now, or on the label
    // already there; an empty comment removes it.
    const std::string *commentAt(uint16_t address) const;
    void setComment(uint16_t address, const std::string &comment);

    // The label for a CPU address: the one at what is mapped there now, else one for the plain CPU address
    const std::string *find(uint16_t address) const;
    // The CPU address of a label; nullopt if unknown or if its PRG-ROM bank is not mapped in right now
    std::optional<uint16_t> find(const std::string &name) const;
    // nullptr if there is no such label. A name defined in several PRG-ROM banks (the same code in each bank) gives the
    // one that is mapped in right now; if none is, the one defined last.
    const Location *locationOf(const std::string &name) const;
    // The label at a location (whatever bank is mapped now), or the nearest one before it within MAXIMUM bytes as
    // "name+3"; "" if there is none
    const std::string *findAt(const Location &location) const;
    std::string describeNear(const Location &location, uint32_t maximum = 0x100) const;

    // Reads a label file; the format is recognised from the contents. Returns the number of labels read,
    // -1 if the file cannot be read. *format (if given) names the format that was read.
    int loadFile(const std::string &filename, std::string *format = nullptr);

    size_t size() const;     // labels with a name
    size_t comments() const; // addresses with a comment

    // Saving (lsave / lload, and the warning when quitting with unsaved changes)
    bool save(const std::string &filename) const; // Mesen .mlb
    bool modified() const { return dirty; }       // changed (n command) since the last load/save
    void setModified(bool m) { dirty = m; }
    const std::string &fileName() const { return file; } // where lsave writes without a name
    void setFileName(const std::string &f) { file = f; }

private:
    struct Label {
        std::string name;
        std::string comment; // kept from .mlb files and written back
        uint32_t length = 1; // .mlb "P:1000-1003:table" covers 4 bytes
    };
    int loadMlb(std::istream &in);
    int loadDbg(std::istream &in, const std::string &directory);
    void loadSourceComments(const std::string &name, const std::string &directory,
                            const std::multimap<uint32_t, uint32_t> &lineToOffset);
    int loadVice(std::istream &in);
    void forgetName(const Location &location);

    bool dirty = false;
    std::string file;
    std::map<Location, Label> labels;
    std::map<Location, std::string> sourceComments; // from the assembler's source files (ld65 --dbgfile); never saved
    std::multimap<std::string, Location> byName;    // a name can be in several banks: lookups prefer the mapped one
};
