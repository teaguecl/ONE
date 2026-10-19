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
#include "watches.hpp"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include "core.hpp"
#include "debugger/symbols.hpp"

namespace {

std::string hex(unsigned value, int digits)
{
    char buf[12];
    std::snprintf(buf, sizeof(buf), "%0*X", digits, value);
    return buf;
}

} // namespace

std::vector<uint8_t> Watch::read() const
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < length; i++) {
        if (ppu) {
            bytes.push_back(ppu_debug_read(static_cast<uint16_t>((address + i) & 0x3FFF)));
        } else {
            bytes.push_back(cpu_debug_read(static_cast<uint16_t>(address + i)));
        }
    }
    return bytes;
}

std::string Watch::typeText() const
{
    switch (type) {
    case Type::Byte: return "b";
    case Type::Word: return "w";
    case Type::Signed: return "s";
    case Type::Binary: return "bin";
    case Type::Bytes: return "l " + std::to_string(length);
    }
    return "b";
}

std::string Watch::addressText() const
{
    return (ppu ? "p$" : "$") + hex(address, 4);
}

std::string Watch::valueText(const Symbols &symbols) const
{
    std::vector<uint8_t> bytes = read();
    switch (type) {
    case Type::Byte: return "$" + hex(bytes[0], 2) + " " + std::to_string(bytes[0]);
    case Type::Signed: return "$" + hex(bytes[0], 2) + " " + std::to_string(static_cast<int8_t>(bytes[0]));
    case Type::Word: {
        unsigned value = bytes[0] | (bytes[1] << 8);
        std::string text = "$" + hex(value, 4);
        if (const std::string *label = symbols.find(static_cast<uint16_t>(value))) {
            text += " " + *label; // a pointer: where it points
        }
        return text;
    }
    case Type::Binary: {
        std::string text = "%";
        for (int bit = 7; bit >= 0; bit--) {
            text += ((bytes[0] >> bit) & 1) ? '1' : '0';
        }
        return text;
    }
    case Type::Bytes: {
        std::string text;
        for (uint8_t byte : bytes) {
            text += (text.empty() ? "" : " ") + hex(byte, 2);
        }
        return text;
    }
    }
    return "";
}

std::string Watch::label(const Symbols &symbols) const
{
    if (!name.empty() || ppu) {
        return name;
    }
    const std::string *found = symbols.find(address);
    return found ? *found : std::string();
}

std::string Watch::command() const
{
    std::string text = "watch ";
    text += ppu ? "-p " : "";
    text += "$" + hex(address, 4) + " " + typeText();
    if (!name.empty()) {
        text += " " + name;
    }
    return text;
}

Watches &Watches::instance()
{
    static Watches watches;
    return watches;
}

bool Watches::add(const Watch &watch)
{
    if (watches.size() >= kMax) {
        return false;
    }
    watches.push_back(watch);
    return true;
}

bool Watches::remove(size_t index)
{
    if (index >= watches.size()) {
        return false;
    }
    watches.erase(watches.begin() + static_cast<long>(index));
    return true;
}

bool Watches::save() const
{
    if (fileName.empty()) {
        return true;
    }
    if (watches.empty()) {
        std::remove(fileName.c_str()); // (fine if there was none)
        return true;
    }
    std::ofstream out(fileName);
    out << "# ONE debugger watches (watch commands), loaded when the ROM is debugged again\n";
    for (const Watch &watch : watches) {
        out << watch.command() << "\n";
    }
    return static_cast<bool>(out);
}
