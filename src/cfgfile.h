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
// Plain-text settings files for the emulator (one.cfg): one "key = value" per line.
//   - a line starting with '#' is a comment, and so is " # ..." after a value (so "#" inside a value needs no space)
//   - keys are case-sensitive, values keep their case; blank lines are ignored
// The same format as the debugger's one-debugger.cfg.
#ifndef cfgfile_h_defined
#define cfgfile_h_defined

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *key;
    char *value;
    int line;
    bool used; // looked up by cfg_get(): keys never looked up are reported as unknown
} cfg_entry;

typedef struct {
    char path[512];
    cfg_entry *entries;
    int count;
} cfg_file;

// Reads PATH. Returns false (and an empty cfg) if it cannot be opened; syntax errors are reported as
// "PATH:LINE: ..." and the line is skipped.
bool cfg_load(cfg_file *cfg, const char *path);
void cfg_free(cfg_file *cfg);

// The value of KEY (the last one if it is given twice), or NULL. Marks the key as used.
const char *cfg_get(cfg_file *cfg, const char *key);
int cfg_line(const cfg_file *cfg, const char *key); // line number of KEY, 0 if absent

// Prints "PATH:LINE: unknown setting 'KEY'" for every key no cfg_get() asked for. Returns how many.
int cfg_report_unused(const cfg_file *cfg);

// Problems in a settings file are reported with LOG_WARN(CFG, "PATH:LINE: ...") (log.h).

// Where the emulator looks when --config is not given: ./one.cfg, then $XDG_CONFIG_HOME/one/one.cfg or
// ~/.config/one/one.cfg (%APPDATA%\one\one.cfg on Windows). Writes the first that exists into BUF and returns
// true; false if there is none.
bool cfg_find_default(char *buf, size_t size);

#endif
