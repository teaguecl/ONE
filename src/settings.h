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
// The emulator's settings (one.cfg) as one table: every setting with its type, default, limits and description.
// The table drives everything that deals with settings, so they cannot drift apart:
//   - reading one.cfg (settings_load(): unknown keys and bad values are reported, the default stays)
//   - writing one.cfg with every default and its documentation (settings_write_defaults(): one --write-config)
//   - the menu of the game window (gui/: one page per group, a widget per setting type)
//   - saving the menu's changes back into one.cfg in place, comments and all (settings_save())
//
// Each module owns its settings (input.c, audio.c, video.c, log.c): it describes them in a setting_group and puts
// a value into effect in its apply function. This file only knows the table.
#ifndef settings_h_defined
#define settings_h_defined

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" { // (the menu, gui/, is C++)
#endif

typedef enum {
    SETTING_BOOL,   // on / off (also read: yes/no, true/false, 1/0)
    SETTING_INT,    // a whole number from min to max
    SETTING_CHOICE, // one of choices[] ("" may be a choice: then it means "not set")
    SETTING_TEXT,   // any text
    SETTING_FILE,   // a file name ("" = none)
    SETTING_INPUTS, // controller inputs: "key:period, pad1:b" (any key or gamepad input, or none)
    SETTING_KEYS,   // hotkeys: "key:shift+f12, ..." (keys with shift/ctrl/alt, or none)
} setting_type;

// setting.flags
#define SETTING_RESTART   1 // takes effect the next time ONE starts (the menu says so)
#define SETTING_GAP_ABOVE 2 // one.cfg: an empty line before it (between groups of related settings)
#define SETTING_NO_MENU   4 // not on the menu's pages (one.cfg only: e.g. a setting for one-debugger)

struct setting;
// Checks VALUE and, if it is good, puts it into effect. Returns false (with the reason in ERROR) and changes
// nothing if it is not.
typedef bool (*setting_apply_fn)(const struct setting *s, const char *value, char *error, size_t error_size);

typedef struct setting {
    const char *key;            // "audio.volume"
    setting_type type;          //
    const char *default_value;  // as written in one.cfg
    const char *label;          // in the menu: "Volume"
    const char *section;        // in the menu: a heading above this setting and the ones after it (NULL: none)
    const char *description;    // one.cfg comment and menu tooltip ('\n' = a new comment line; NULL: none)
    int min, max;               // SETTING_INT
    const char *const *choices; // SETTING_CHOICE: NULL-terminated
    unsigned flags;             // SETTING_RESTART | SETTING_GAP_ABOVE
    setting_apply_fn apply;     //
    int id;                     // for apply(): which one (e.g. port * 8 + button)
} setting;

typedef struct {
    const char *name;  // one.cfg heading: "# --- sound ---"
    const char *title; // menu page: "Sound"
    const char *intro; // one.cfg comment under the heading ('\n' separates lines; NULL: none)
    const setting *settings;
    int count;
} setting_group;

// The groups, in one.cfg order. (A module's table is in the module: input_setting_groups() ...)
int settings_group_count(void);
const setting_group *settings_group(int index);
const setting *settings_find(const char *key);

// Read the settings file: PATH (--config), or NULL for the default places (cfg_find_default(); none found: the
// defaults). Every setting is put into effect: the file's value, or the default when the file has none or a bad
// one (reported as "PATH:LINE: ..."). Returns the number of mistakes, or -1 if PATH was given but cannot be read.
// Remembers the file for settings_save().
int settings_load(const char *path);
// No file at all (the libretro core, which has its own options): every setting at its default.
int settings_load_defaults(void);

// The current value of a setting (as text; valid until that setting changes: copy it to keep it), and changing
// it: SETTING checks and applies it. Returns false (and changes nothing) if the value is bad. A successful change is
// saved by the next settings_save().
const char *settings_value(const setting *s);
const char *settings_get(const char *key);
bool settings_set(const setting *s, const char *value, char *error, size_t error_size);
bool settings_set_key(const char *key, const char *value, char *error, size_t error_size);
bool settings_changed(void); // changes that settings_save() has not written yet

// Write the changed settings into the settings file: the one settings_load() read, or, if it read none, the
// user's file (settings_user_path()), created with every default first. Lines that are not changed (comments,
// order, alignment) stay as they are; a setting the file does not have yet is added at the end of the file.
bool settings_save(char *error, size_t error_size);
const char *settings_path(void); // where settings_save() writes: the file read, else settings_user_path()
bool settings_user_path(char *buf, size_t size);  // ~/.config/one/one.cfg ($XDG_CONFIG_HOME, %APPDATA% on Windows)
void settings_user_dir(char *buf, size_t size);   // its directory: ~/.config/one (for files of ONE's own: cheats/...)
void settings_make_parent_dirs(const char *path); // mkdir -p of PATH's directory part

// one --write-config: a settings file with every setting at its default, documented
void settings_write_defaults(FILE *out);

// For the modules' apply functions
bool settings_parse_bool(const char *text, bool *out);

#ifdef __cplusplus
}
#endif

#endif
