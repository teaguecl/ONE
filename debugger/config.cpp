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
#include "config.hpp"
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {
std::string trim(const std::string &s)
{
    size_t first = s.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return std::string();
    }
    size_t last = s.find_last_not_of(" \t\r");
    return s.substr(first, last - first + 1);
}

bool exists(const std::string &path)
{
    std::ifstream in(path);
    return static_cast<bool>(in);
}
} // namespace

bool Config::load(const std::string &path, std::vector<std::string> &messages)
{
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    file = path;
    std::string raw;
    int lineNumber = 0;
    while (std::getline(in, raw)) {
        lineNumber++;
        std::string line = trim(raw);
        // '#' starts a comment at the beginning of a line, or as a word of its own ("... # comment"), so that
        // #RRGGBB color values are not mistaken for comments.
        if (!line.empty() && line[0] == '#') {
            continue;
        }
        for (size_t i = 1; i < line.size(); i++) {
            if (line[i] == '#' && (line[i - 1] == ' ' || line[i - 1] == '\t') &&
                (i + 1 == line.size() || line[i + 1] == ' ' || line[i + 1] == '\t')) {
                line = trim(line.substr(0, i));
                break;
            }
        }
        if (line.empty()) {
            continue;
        }
        size_t equals = line.find('=');
        if (equals == std::string::npos) {
            messages.push_back(path + ":" + std::to_string(lineNumber) + ": expected 'key = value'");
            continue;
        }
        std::string key = trim(line.substr(0, equals));
        std::string value = trim(line.substr(equals + 1));
        if (key.empty()) {
            messages.push_back(path + ":" + std::to_string(lineNumber) + ": missing key before '='");
            continue;
        }
        entries[key] = {value, lineNumber};
    }
    return true;
}

const std::string *Config::get(const std::string &key) const
{
    auto it = entries.find(key);
    if (it == entries.end()) {
        return nullptr;
    }
    used.insert(key);
    return &it->second.value;
}

int Config::line(const std::string &key) const
{
    auto it = entries.find(key);
    return (it == entries.end()) ? 0 : it->second.line;
}

std::vector<std::string> Config::unusedKeys() const
{
    std::vector<std::string> keys;
    for (const auto &entry : entries) {
        if (!used.count(entry.first)) {
            keys.push_back(entry.first);
        }
    }
    return keys;
}

std::string Config::findDefault()
{
    if (exists("one-debugger.cfg")) {
        return "one-debugger.cfg";
    }
    std::string dir;
#ifdef _WIN32
    if (const char *appdata = std::getenv("APPDATA"); appdata && *appdata) { // (as the core: %APPDATA%\one\)
        dir = appdata;
    }
#else
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        dir = xdg;
    } else if (const char *home = std::getenv("HOME"); home && *home) {
        dir = std::string(home) + "/.config";
    }
#endif
    if (!dir.empty() && exists(dir + "/one/debugger.cfg")) {
        return dir + "/one/debugger.cfg";
    }
    return std::string();
}
