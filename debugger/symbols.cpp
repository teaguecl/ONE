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
#include "symbols.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <map>
#include <istream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "core.hpp"

namespace {

std::string hexText(uint32_t value, int digits)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%0*X", digits, value);
    return buf;
}

std::optional<uint32_t> parseHexField(const std::string &text)
{
    std::string digits = text;
    if (digits.rfind("0x", 0) == 0 || digits.rfind("0X", 0) == 0) {
        digits.erase(0, 2);
    } else if (!digits.empty() && digits[0] == '$') {
        digits.erase(0, 1);
    }
    if (digits.empty()) {
        return std::nullopt;
    }
    char *end = nullptr;
    unsigned long value = std::strtoul(digits.c_str(), &end, 16);
    if (*end != '\0') {
        return std::nullopt;
    }
    return static_cast<uint32_t>(value);
}

// A CPU address from a file that knows nothing about banks (ld65 -Ln, VICE): RAM is unambiguous; anything else
// stays a plain CPU address, so a label at $8000 names whatever bank is mapped there.
Symbols::Location fromPlainAddress(uint32_t address)
{
    address &= 0xFFFF;
    if (address < 0x2000) {
        return {Symbols::Kind::InternalRam, address & 0x7FF};
    }
    if (address >= 0x6000 && address < 0x8000) {
        return {Symbols::Kind::CartRam, address - 0x6000};
    }
    return {Symbols::Kind::Cpu, address};
}

// ld65 debug info: 'key=value,key="text, with commas",...'
std::map<std::string, std::string> dbgFields(const std::string &text)
{
    std::map<std::string, std::string> fields;
    size_t i = 0;
    while (i < text.size()) {
        size_t eq = text.find('=', i);
        if (eq == std::string::npos) {
            break;
        }
        std::string key = text.substr(i, eq - i);
        std::string value;
        size_t j = eq + 1;
        if (j < text.size() && text[j] == '"') {
            size_t close = text.find('"', j + 1);
            if (close == std::string::npos) {
                close = text.size();
            }
            value = text.substr(j + 1, close - j - 1);
            j = close + 1;
        } else {
            size_t comma = text.find(',', j);
            if (comma == std::string::npos) {
                comma = text.size();
            }
            value = text.substr(j, comma - j);
            j = comma;
        }
        fields[key] = value;
        i = (j < text.size() && text[j] == ',') ? j + 1 : j;
    }
    return fields;
}

} // namespace

// --- where things are -------------------------------------------------------------------------------------------

Symbols::Location Symbols::locate(uint16_t address)
{
    if (address < 0x2000) {
        return {Kind::InternalRam, address & 0x7FFu};
    }
    if (address >= 0x6000 && address < 0x8000) {
        return {Kind::CartRam, address - 0x6000u};
    }
    if (address >= 0x8000) {
        int32_t offset = mapper_debug_prg_offset(address);
        if (offset >= 0) {
            return {Kind::PrgRom, static_cast<uint32_t>(offset)};
        }
    }
    return {Kind::Cpu, address};
}

std::optional<uint16_t> Symbols::visibleAt(const Location &location)
{
    switch (location.kind) {
    case Kind::InternalRam:
        return (location.offset < 0x800) ? std::optional<uint16_t>(static_cast<uint16_t>(location.offset))
                                         : std::nullopt;
    case Kind::CartRam:
        return (location.offset < 0x2000) ? std::optional<uint16_t>(static_cast<uint16_t>(0x6000 + location.offset))
                                          : std::nullopt;
    case Kind::Cpu:
        return (location.offset <= 0xFFFF) ? std::optional<uint16_t>(static_cast<uint16_t>(location.offset))
                                           : std::nullopt;
    case Kind::PrgRom:
        // highest address first: a 16KB NROM game mirrored at $8000 and $C000 is linked for $C000
        for (int page = 0xFF; page >= 0x80; page--) {
            int32_t base = mapper_debug_prg_offset(static_cast<uint16_t>(page << 8));
            if (base >= 0 && location.offset >= static_cast<uint32_t>(base) &&
                location.offset < static_cast<uint32_t>(base) + 0x100) {
                return static_cast<uint16_t>((page << 8) + (location.offset - static_cast<uint32_t>(base)));
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::string Symbols::describe(const Location &location)
{
    switch (location.kind) {
    case Kind::PrgRom: return "PRG-ROM $" + hexText(location.offset, 5);
    case Kind::InternalRam: return "RAM $" + hexText(location.offset, 4);
    case Kind::CartRam: return "cartridge RAM $" + hexText(0x6000 + location.offset, 4);
    case Kind::Cpu: return "$" + hexText(location.offset, 4);
    }
    return std::string();
}

// --- the labels ---------------------------------------------------------------------------------------------------

void Symbols::forgetName(const Location &location)
{
    auto it = labels.find(location);
    if (it == labels.end()) {
        return;
    }
    auto range = byName.equal_range(it->second.name);
    for (auto named = range.first; named != range.second; ++named) {
        if (named->second == location) {
            byName.erase(named);
            break;
        }
    }
}

void Symbols::add(uint16_t address, const std::string &name)
{
    const Location location = locate(address);
    auto it = labels.find(location);
    if (it != labels.end()) { // keep the comment (and range) that is there
        Label label = it->second;
        add(location, name, label.comment, label.length);
    } else {
        add(location, name);
    }
}

void Symbols::add(const Location &location, const std::string &name, const std::string &comment, uint32_t length)
{
    forgetName(location);
    labels[location] = Label{name, comment, length == 0 ? 1 : length};
    if (!name.empty()) {
        byName.emplace(name, location);
    }
}

bool Symbols::remove(uint16_t address)
{
    for (const Location &location : {locate(address), Location{Kind::Cpu, address}}) {
        auto it = labels.find(location);
        if (it != labels.end() && !it->second.name.empty()) {
            forgetName(location);
            if (it->second.comment.empty()) {
                labels.erase(it);
            } else {
                it->second.name.clear(); // the comment stays
            }
            return true;
        }
    }
    return false;
}

const std::string *Symbols::commentAt(uint16_t address) const
{
    const Location here = locate(address);
    for (const Location &location : {here, Location{Kind::Cpu, address}}) {
        auto it = labels.find(location);
        if (it != labels.end() && !it->second.comment.empty()) {
            return &it->second.comment;
        }
        if (here.kind == Kind::Cpu) {
            break;
        }
    }
    auto source = sourceComments.find(here); // (your own comment, above, wins)
    return source != sourceComments.end() ? &source->second : nullptr;
}

void Symbols::setComment(uint16_t address, const std::string &comment)
{
    // on the entry that is already there (a label for the plain CPU address counts), else on what is mapped there
    Location location = locate(address);
    if (!labels.count(location) && labels.count(Location{Kind::Cpu, address})) {
        location = Location{Kind::Cpu, address};
    }
    auto it = labels.find(location);
    if (it == labels.end()) {
        if (!comment.empty()) {
            labels[location] = Label{std::string(), comment, 1};
        }
    } else if (comment.empty() && it->second.name.empty()) {
        labels.erase(it); // nothing left there
    } else {
        it->second.comment = comment;
    }
    dirty = true;
}

void Symbols::clear()
{
    labels.clear();
    byName.clear();
    sourceComments.clear();
}

const std::string *Symbols::find(uint16_t address) const
{
    const Location here = locate(address);
    for (const Location &location : {here, Location{Kind::Cpu, address}}) {
        auto it = labels.find(location);
        if (it != labels.end() && !it->second.name.empty()) {
            return &it->second.name;
        }
        if (here.kind == Kind::Cpu) {
            break;
        }
    }
    return nullptr;
}

const std::string *Symbols::findAt(const Location &location) const
{
    auto it = labels.find(location);
    return it != labels.end() && !it->second.name.empty() ? &it->second.name : nullptr;
}

std::string Symbols::describeNear(const Location &location, uint32_t maximum) const
{
    auto it = labels.upper_bound(location); // the first label after it: the one before that is the candidate
    while (it != labels.begin()) {
        --it;
        if (it->first.kind != location.kind || location.offset - it->first.offset > maximum) {
            return std::string();
        }
        if (it->second.name.empty()) {
            continue;
        }
        const uint32_t distance = location.offset - it->first.offset;
        if (distance == 0) {
            return it->second.name;
        }
        char text[24];
        std::snprintf(text, sizeof(text), "+%u", static_cast<unsigned>(distance));
        return it->second.name + text;
    }
    return std::string();
}

const Symbols::Location *Symbols::locationOf(const std::string &name) const
{
    auto range = byName.equal_range(name);
    if (range.first == range.second && name.size() > 1 && name[0] == '.') {
        range = byName.equal_range(name.substr(1)); // ".name" as written in ld65 label files
    }
    if (range.first == range.second) {
        return nullptr;
    }
    const Location *last = nullptr;
    for (auto it = range.first; it != range.second; ++it) {
        if (visibleAt(it->second)) {
            return &it->second; // a name that is in several banks: the bank that is mapped in
        }
        last = &it->second;
    }
    return last; // none is mapped in: the last one defined(as before names could be in more than one bank)
}

std::optional<uint16_t> Symbols::find(const std::string &name) const
{
    const Location *location = locationOf(name);
    return location ? visibleAt(*location) : std::nullopt;
}

size_t Symbols::comments() const
{
    size_t count = 0;
    for (const auto &entry : labels) {
        count += entry.second.comment.empty() ? 0 : 1;
    }
    return count;
}

size_t Symbols::size() const
{
    size_t count = 0;
    for (const auto &entry : labels) {
        count += entry.second.name.empty() ? 0 : 1;
    }
    return count;
}

// --- files ----------------------------------------------------------------------------------------------------------

int Symbols::loadFile(const std::string &filename, std::string *format)
{
    std::ifstream in(filename);
    if (!in) {
        return -1;
    }
    // recognise the format from the first line that says anything
    std::string line, kind = "mlb";
    while (std::getline(in, line)) {
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) {
            continue;
        }
        if (line.compare(start, 7, "version") == 0 && line.find("major=") != std::string::npos) {
            kind = "dbg";
        } else if (line.compare(start, 3, "al ") == 0) {
            kind = "vice";
        }
        break;
    }
    in.clear();
    in.seekg(0);
    if (format) {
        *format = (kind == "dbg") ? "ld65 debug info" : (kind == "vice") ? "ld65 -Ln / VICE labels" : "Mesen labels";
    }
    size_t slash = filename.find_last_of("/\\");
    const std::string directory = slash == std::string::npos ? std::string(".") : filename.substr(0, slash);
    return (kind == "dbg") ? loadDbg(in, directory) : (kind == "vice") ? loadVice(in) : loadMlb(in);
}

// "al 00C000 .label" (ld65 -Ln, VICE): CPU addresses only
int Symbols::loadVice(std::istream &in)
{
    int count = 0;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string tag, addressText, name;
        if (!(fields >> tag >> addressText >> name) || tag != "al") {
            continue;
        }
        auto address = parseHexField(addressText);
        if (!address) {
            continue;
        }
        if (name.size() > 1 && name[0] == '.') {
            name.erase(0, 1); // ld65 -Ln writes labels as ".name"
        }
        add(fromPlainAddress(*address), name);
        count++;
    }
    return count;
}

// Mesen: "P:1C010:name:comment", "R:0075:name", "P:8000-8003:table" (Mesen 2 spells the types NesPrgRom, ...)
int Symbols::loadMlb(std::istream &in)
{
    static const std::map<std::string, Kind> kTypes = {
        {"P", Kind::PrgRom},      {"NesPrgRom", Kind::PrgRom},
        {"R", Kind::InternalRam}, {"NesInternalRam", Kind::InternalRam},
        {"S", Kind::CartRam},     {"NesSaveRam", Kind::CartRam},
        {"W", Kind::CartRam},     {"NesWorkRam", Kind::CartRam},
        {"G", Kind::Cpu},         {"NesMemory", Kind::Cpu},
    };
    int count = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        size_t c1 = line.find(':');
        size_t c2 = (c1 == std::string::npos) ? c1 : line.find(':', c1 + 1);
        if (c2 == std::string::npos) {
            continue;
        }
        auto type = kTypes.find(line.substr(0, c1));
        if (type == kTypes.end()) {
            continue; // CHR, palette, ... : nothing the debugger shows
        }
        size_t c3 = line.find(':', c2 + 1);
        std::string addressText = line.substr(c1 + 1, c2 - c1 - 1);
        std::string name = line.substr(c2 + 1, (c3 == std::string::npos) ? std::string::npos : c3 - c2 - 1);
        std::string comment = (c3 == std::string::npos) ? std::string() : line.substr(c3 + 1);
        for (size_t i = 0; (i = comment.find("\\n", i)) != std::string::npos; i++) {
            comment.replace(i, 2, "\n");
        }
        size_t dash = addressText.find('-');
        auto first = parseHexField(addressText.substr(0, dash));
        auto last = (dash == std::string::npos) ? first : parseHexField(addressText.substr(dash + 1));
        if (!first || !last || *last < *first || (name.empty() && comment.empty())) {
            continue;
        }
        const Location location{type->second, *first};
        auto existing = labels.find(location);
        if (name.empty() && existing != labels.end()) {
            existing->second.comment = comment; // a comment for a label that is already there
            continue;
        }
        add(location, name, comment, *last - *first + 1);
        count += name.empty() ? 0 : 1;
    }
    return count;
}

namespace {

// The ';' comment of a source line ("" if none); not inside a string or character constant
std::string sourceComment(const std::string &text)
{
    char quote = 0;
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (quote) {
            quote = (c == quote) ? 0 : quote;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == ';') {
            size_t first = text.find_first_not_of(" \t\r", i + 1);
            size_t last = text.find_last_not_of(" \t\r");
            return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        }
    }
    return std::string();
}

} // namespace

// Comments for the code the assembler listed: read the source file the debug info names (relative to where ld65
// ran, so tried here and in the folders above the .dbg). A comment ending in "-->" goes on in the next lines' comments.
void Symbols::loadSourceComments(const std::string &name, const std::string &directory,
                                 const std::multimap<uint32_t, uint32_t> &lineToOffset)
{
    std::ifstream in;
    for (const std::string &base : {std::string(), directory + "/", directory + "/../", directory + "/../../"}) {
        in.open(base + name);
        if (in) {
            break;
        }
        in.clear();
    }
    if (!in) {
        return;
    }
    std::vector<std::string> comment; // by line number - 1
    std::string text;
    while (std::getline(in, text)) {
        comment.push_back(sourceComment(text));
    }
    const std::string more = "-->";
    auto continues = [&more](const std::string &c) {
        return c.size() >= more.size() && c.compare(c.size() - more.size(), more.size(), more) == 0;
    };
    for (const auto &entry : lineToOffset) {
        if (entry.first == 0 || entry.first > comment.size() || comment[entry.first - 1].empty()) {
            continue;
        }
        std::string joined = comment[entry.first - 1];
        for (size_t next = entry.first;
             continues(joined) && next < comment.size() && !comment[next].empty() && next < entry.first + 6; next++) {
            joined.erase(joined.size() - more.size());
            joined += (joined.empty() || joined.back() == ' ' ? "" : " ") + comment[next];
        }
        if (continues(joined)) {
            joined.erase(joined.size() - more.size());
        }
        sourceComments[Location{Kind::PrgRom, entry.second}] = joined;
    }
}

// ld65 --dbgfile: labels in ROM segments become PRG-ROM offsets (bank-exact); labels in RAM keep their address
int Symbols::loadDbg(std::istream &in, const std::string &directory)
{
    struct Segment {
        uint32_t start = 0;
        uint32_t size = 0;
        std::optional<uint32_t> fileOffset;
        bool readOnly = false;
    };
    std::map<std::string, Segment> segments;
    std::vector<std::map<std::string, std::string>> symbols;
    std::map<std::string, std::string> fileNames;                           // file id -> name
    std::map<std::string, std::map<std::string, std::string>> spans, lines; // id -> fields
    std::vector<std::string> lineOrder;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        size_t tab = line.find_first_of(" \t");
        if (tab == std::string::npos) {
            continue;
        }
        std::string what = line.substr(0, tab);
        if (what != "seg" && what != "sym" && what != "file" && what != "span" && what != "line") {
            continue;
        }
        auto fields = dbgFields(line.substr(tab + 1));
        if (what == "file") {
            fileNames[fields["id"]] = fields["name"];
            continue;
        }
        if (what == "span") {
            spans[fields["id"]] = std::move(fields);
            continue;
        }
        if (what == "line") {
            lineOrder.push_back(fields["id"]);
            lines[fields["id"]] = std::move(fields);
            continue;
        }
        if (what == "sym") {
            symbols.push_back(std::move(fields));
            continue;
        }
        Segment seg;
        seg.start = parseHexField(fields["start"]).value_or(0);
        seg.size = parseHexField(fields["size"]).value_or(0);
        seg.readOnly = (fields["type"] == "ro");
        if (fields.count("ooffs")) {
            seg.fileOffset = static_cast<uint32_t>(std::strtoul(fields["ooffs"].c_str(), nullptr, 0));
        }
        segments[fields["id"]] = seg;
    }
    // the iNES header is in the output file if something 16 bytes long starts it (the HEADER segment)
    uint32_t header = 0;
    for (const auto &entry : segments) {
        if (entry.second.fileOffset && *entry.second.fileOffset == 0 && entry.second.size == 16) {
            header = 16;
        }
    }
    // the ';' comments of the source lines that produced code: file id -> (line number -> PRG-ROM offset)
    std::map<std::string, std::multimap<uint32_t, uint32_t>> commentable; // (a shared .inc is in several banks)
    for (const std::string &id : lineOrder) {
        const auto &fields = lines[id];
        if (!fields.count("span") || !fields.count("file") || !fields.count("line")) {
            continue;
        }
        auto span = spans.find(fields.at("span").substr(0, fields.at("span").find('+'))); // (a macro has several)
        if (span == spans.end()) {
            continue;
        }
        auto seg = segments.find(span->second["seg"]);
        auto start = std::optional<uint32_t>(
            static_cast<uint32_t>(std::strtoul(span->second["start"].c_str(), nullptr, 0))); // (decimal)
        if (seg == segments.end() || !seg->second.readOnly || !seg->second.fileOffset || !start) {
            continue;
        }
        uint32_t offset = *seg->second.fileOffset + *start;
        if (offset < header) {
            continue;
        }
        commentable[fields.at("file")].emplace(
            static_cast<uint32_t>(std::strtoul(fields.at("line").c_str(), nullptr, 10)), offset - header);
    }
    for (const auto &entry : commentable) {
        auto name = fileNames.find(entry.first);
        if (name != fileNames.end()) {
            loadSourceComments(name->second, directory, entry.second);
        }
    }
    int count = 0;
    for (auto &sym : symbols) {
        const std::string &name = sym["name"];
        if (sym["type"] != "lab" || name.empty() || name[0] == '@') { // labels only; no cheap locals (@loop)
            continue;
        }
        auto value = parseHexField(sym["val"]);
        if (!value || *value > 0xFFFF) {
            continue;
        }
        auto seg = sym.count("seg") ? segments.find(sym["seg"]) : segments.end();
        if (seg != segments.end() && seg->second.readOnly) {
            if (*value >= 0x6000 && *value < 0x8000) {
                // code that is stored in ROM but runs from cart RAM (ld65 `load = ROM, run = RAM`): it is at its run
                // address, like any label in RAM (nothing is mapped there in the ROM)
                add(fromPlainAddress(*value), name);
                count++;
                continue;
            }
            if (*value < 0x8000 || !seg->second.fileOffset ||
                *seg->second.fileOffset + (*value - seg->second.start) < header) {
                continue; // header or CHR data: not CPU-visible code
            }
            uint32_t offset = *seg->second.fileOffset + (*value - seg->second.start) - header;
            add(Location{Kind::PrgRom, offset}, name); // (ld65's size is the instruction's or .proc's, not data)
        } else {
            add(fromPlainAddress(*value), name);
        }
        count++;
    }
    return count;
}

bool Symbols::save(const std::string &filename) const
{
    std::ofstream out(filename);
    if (!out) {
        return false;
    }
    const char cartRam = mapper_debug_has_battery() ? 'S' : 'W';
    const bool banked = mapper_debug_prg_banked();
    // write in Mesen's order; a plain CPU address in ROM is exact when the mapper has no banks
    std::map<Location, const Label *> sorted;
    for (const auto &entry : labels) {
        Location location = entry.first;
        if (location.kind == Kind::Cpu && location.offset >= 0x8000 && !banked) {
            int32_t offset = mapper_debug_prg_offset(static_cast<uint16_t>(location.offset));
            if (offset >= 0) {
                location = Location{Kind::PrgRom, static_cast<uint32_t>(offset)};
            }
        }
        if (!sorted.count(location)) {
            sorted[location] = &entry.second;
        }
    }
    for (const auto &entry : sorted) {
        const Location &location = entry.first;
        const Label &label = *entry.second;
        char type = 'G';
        switch (location.kind) {
        case Kind::PrgRom: type = 'P'; break;
        case Kind::InternalRam: type = 'R'; break;
        case Kind::CartRam: type = cartRam; break;
        case Kind::Cpu: type = 'G'; break;
        }
        out << type << ':' << hexText(location.offset, 4);
        if (label.length > 1) {
            out << '-' << hexText(location.offset + label.length - 1, 4);
        }
        out << ':' << label.name;
        if (!label.comment.empty()) {
            std::string comment = label.comment;
            for (size_t i = 0; (i = comment.find('\n', i)) != std::string::npos; i += 2) {
                comment.replace(i, 1, "\\n");
            }
            out << ':' << comment;
        }
        out << '\n';
    }
    return static_cast<bool>(out);
}
