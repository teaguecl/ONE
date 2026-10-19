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
// The debugger's configuration file: plain text, one "key = value" per line, '#' starts a comment.
// Keys are dotted names (color.text, ...). Later lines override earlier ones.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

class Config {
public:
    // Reads the file. Returns false if it cannot be opened; syntax problems are added to 'messages'.
    bool load(const std::string &path, std::vector<std::string> &messages);

    // The value for key, or nullptr. Marks the key as used (see unusedKeys()).
    const std::string *get(const std::string &key) const;
    int line(const std::string &key) const;      // line number of a key (for messages), 0 if unknown
    std::vector<std::string> unusedKeys() const; // keys nobody asked for: probably misspelled
    const std::string &path() const { return file; }

    // Where the debugger looks when --config is not given: ./one-debugger.cfg, then the user's config directory
    // ($XDG_CONFIG_HOME/one/debugger.cfg or ~/.config/one/debugger.cfg). Empty if none exists.
    static std::string findDefault();

private:
    struct Entry {
        std::string value;
        int line = 0;
    };
    std::string file;
    std::map<std::string, Entry> entries;
    mutable std::set<std::string> used;
};
