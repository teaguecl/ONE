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
// The emulator's side of frontend.h (the rest is in video.c, input.c and savestate.c, next to what it uses)
#include "frontend.h"
#include "cheats.h"
#include "settings.h"
#include "version.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include "audio.h"
#include "log.h"
#include "savestate.h"
#include "script.h"
#include "video.h"

static fe_menu_fn menu = NULL;
static char rom_path[1024];
static char next_rom[1024];

void fe_set_menu(fe_menu_fn fn)
{
    menu = fn;
}

bool fe_menu_installed(void)
{
    return menu != NULL;
}

bool fe_menu_available(void)
{
    return menu != NULL && !video_is_headless();
}

const char *fe_version(void)
{
    return one_version();
}

fe_action fe_run_menu(bool game_running)
{
    if (!fe_menu_available()) {
        return FE_RESUME;
    }
    LOG_DEBUG(EMU, "menu opened");
    audio_pause(true);
    fe_action action = menu(game_running);
    video_menu_closed();
    audio_pause(false);
    if (settings_changed()) {
        char error[600];
        if (!settings_save(error, sizeof(error))) {
            LOG_WARN(CFG, "the settings were not saved: %s", error);
            video_overlay("Settings not saved");
        }
    }
    LOG_DEBUG(EMU, "menu closed (%d)", (int)action);
    return action;
}

// --- recently opened games and scripts --------------------------------------------------------------------------
// Lists of full paths, newest first, in $XDG_STATE_HOME/one/ (~/.local/state/one/; %APPDATA%\one\ on Windows):
// recent.txt (games) and recent_scripts.txt, one a line. Only what is opened with a window and a menu is added, so
// headless runs (tests, scripts) leave the lists alone.

typedef struct {
    const char *file;
    char paths[FE_RECENT_MAX][1024];
    int n; // -1: not read yet
} recent_list;

static recent_list recent_lists[2] = {{"recent.txt", {{0}}, -1}, {"recent_scripts.txt", {{0}}, -1}};

static bool recent_file(const recent_list *list, char *path, size_t size, bool create_dir)
{
    char dir[1024];
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata == NULL || appdata[0] == '\0') {
        return false;
    }
    snprintf(dir, sizeof(dir), "%s\\one", appdata);
#else
    const char *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (state != NULL && state[0] != '\0') {
        snprintf(dir, sizeof(dir), "%s/one", state);
    } else if (home != NULL && home[0] != '\0') {
        snprintf(dir, sizeof(dir), "%s/.local/state/one", home);
    } else {
        return false;
    }
#endif
    if (create_dir) { // each directory of the path, as mkdir -p
        for (char *p = dir + 1; *p != '\0'; p++) {
            if (*p == '/' || *p == '\\') {
                char c = *p;
                *p = '\0';
#ifdef _WIN32
                (void)_mkdir(dir);
#else
                (void)mkdir(dir, 0755);
#endif
                *p = c;
            }
        }
#ifdef _WIN32
        if (_mkdir(dir) != 0 && errno != EEXIST) {
#else
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
#endif
            return false;
        }
    }
    snprintf(path, size, "%s/%s", dir, list->file);
    return true;
}

static recent_list *recent_read(fe_recent_list which)
{
    recent_list *list = &recent_lists[which == FE_RECENT_SCRIPTS ? 1 : 0];
    if (list->n >= 0) {
        return list;
    }
    list->n = 0;
    char file[1100];
    if (!recent_file(list, file, sizeof(file), false)) {
        return list;
    }
    FILE *in = fopen(file, "r");
    if (in == NULL) {
        return list;
    }
    char line[1024];
    while (list->n < FE_RECENT_MAX && fgets(line, sizeof(line), in) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] != '\0') {
            snprintf(list->paths[list->n++], sizeof(list->paths[0]), "%s", line);
        }
    }
    fclose(in);
    return list;
}

static void recent_write(const recent_list *list)
{
    char file[1100];
    if (!recent_file(list, file, sizeof(file), true)) {
        return;
    }
    FILE *out = fopen(file, "w");
    if (out == NULL) {
        LOG_WARN(EMU, "cannot write %s: %s", file, strerror(errno));
        return;
    }
    for (int i = 0; i < list->n; i++) {
        fprintf(out, "%s\n", list->paths[i]);
    }
    fclose(out);
}

static void recent_add(fe_recent_list which, const char *path)
{
    char full[1024];
#ifdef _WIN32
    if (_fullpath(full, path, sizeof(full)) == NULL) {
#else
    char resolved[PATH_MAX];
    if (realpath(path, resolved) != NULL) {
        if (strlen(resolved) >= sizeof(full)) {
            return; // (too long for the list)
        }
        snprintf(full, sizeof(full), "%.1023s", resolved);
    } else {
#endif
        snprintf(full, sizeof(full), "%s", path);
    }
    recent_list *list = recent_read(which);
    int found = list->n < FE_RECENT_MAX ? list->n : FE_RECENT_MAX - 1; // (the oldest drops off)
    for (int i = 0; i < list->n; i++) {
        if (strcmp(list->paths[i], full) == 0) {
            found = i;
            break;
        }
    }
    if (found == list->n) {
        list->n++;
    }
    memmove(list->paths[1], list->paths[0], (size_t)found * sizeof(list->paths[0]));
    snprintf(list->paths[0], sizeof(list->paths[0]), "%s", full);
    recent_write(list);
}

void fe_recent_add(fe_recent_list which, const char *path)
{
    if (path != NULL && path[0] != '\0') {
        recent_add(which, path);
    }
}

int fe_recent_count(fe_recent_list which)
{
    return recent_read(which)->n;
}

const char *fe_recent(fe_recent_list which, int index)
{
    recent_list *list = recent_read(which);
    return (index >= 0 && index < list->n) ? list->paths[index] : "";
}

void fe_recent_clear(fe_recent_list which)
{
    recent_list *list = recent_read(which);
    list->n = 0;
    recent_write(list);
}

// --- Lua scripts --------------------------------------------------------------------------------------------------

int fe_script_count(void)
{
    return script_count();
}

const char *fe_script_name(int index)
{
    return script_name(index);
}

const char *fe_script_path(int index)
{
    return script_path(index);
}

bool fe_script_load(const char *path)
{
    script_clear_error();
    bool ok = script_load(path);
    if (ok && fe_menu_available()) {
        recent_add(FE_RECENT_SCRIPTS, path);
    }
    return ok;
}

bool fe_script_stop(int index)
{
    return script_stop(index);
}

const char *fe_script_error(void)
{
    return script_last_error();
}

void fe_note_rom(const char *path)
{
    snprintf(rom_path, sizeof(rom_path), "%s", path != NULL ? path : "");
    if (path != NULL && path[0] != '\0' && fe_menu_available()) {
        recent_add(FE_RECENT_GAMES, path);
    }
}

const char *fe_rom_path(void)
{
    return rom_path;
}

void fe_set_next_rom(const char *path)
{
    snprintf(next_rom, sizeof(next_rom), "%s", path != NULL ? path : "");
}

const char *fe_next_rom(void)
{
    return next_rom;
}

int fe_slot(void)
{
    return savestate_slot();
}

void fe_set_slot(int slot)
{
    savestate_set_slot(slot);
}

bool fe_slot_time(int slot, char *text, size_t text_size)
{
    return savestate_slot_time(slot, text, text_size);
}

bool fe_screenshot(char *saved, size_t saved_size)
{
    return video_screenshot(NULL, saved, saved_size);
}

bool fe_fps_shown(void)
{
    return video_fps_shown();
}

void fe_set_fps(bool show)
{
    video_set_fps(show);
}

bool fe_lag_counter_shown(void)
{
    return video_lag_counter_shown();
}

void fe_set_lag_counter(bool show)
{
    video_set_lag_counter(show);
}

// --- Game Genie (cheats.h) ------------------------------------------------------------------------------------

const char *fe_gg_game(void)
{
    return cheats_db_game();
}

const char *fe_log_destination(void)
{
    return log_destination();
}

const char *fe_gg_file(void)
{
    return cheats_file();
}

bool fe_gg_enabled(void)
{
    return cheats_enabled();
}

void fe_gg_set_enabled(bool on)
{
    cheats_set_enabled(on);
}

int fe_gg_count(void)
{
    return cheats_count();
}

bool fe_gg_entry(int index, char *code, size_t code_size, char *description, size_t description_size, bool *on)
{
    const cheat_entry *e = cheats_get(index);
    if (e == NULL) {
        return false;
    }
    char text[9];
    cheats_encode(&e->code, text);
    snprintf(code, code_size, "%s", text);
    snprintf(description, description_size, "%s", e->description);
    *on = e->on;
    return true;
}

void fe_gg_set_on(int index, bool on)
{
    cheats_set_on(index, on);
}

void fe_gg_delete(int index)
{
    cheats_delete(index);
}

bool fe_gg_describe(const char *code, char *text, size_t text_size)
{
    cheat_code c;
    if (!cheats_decode(code, &c)) {
        snprintf(text, text_size, "not a code yet: 6 or 8 of A P Z L G I T Y E O X U K S V N");
        return false;
    }
    if (c.has_compare) {
        snprintf(text, text_size, "$%04X = %02X if it reads %02X", c.address, c.value, c.compare);
    } else {
        snprintf(text, text_size, "$%04X = %02X", c.address, c.value);
    }
    return true;
}

bool fe_gg_add(const char *code, const char *description)
{
    cheat_code c;
    return cheats_decode(code, &c) && cheats_add(&c, description);
}

int fe_gg_db_count(void)
{
    return cheats_db_count();
}

bool fe_gg_db_entry(int index, char *codes, size_t codes_size, char *description, size_t description_size)
{
    return cheats_db_get(index, codes, (int)codes_size, description, (int)description_size);
}

// The list entries of a known cheat's codes ("A+B": each), -1 for one not in the list; returns how many codes
static int db_codes_in_list(int index, int found[8])
{
    char codes[128], description[CHEATS_DESC_SIZE];
    if (!cheats_db_get(index, codes, sizeof(codes), description, sizeof(description))) {
        return 0;
    }
    int n = 0;
    for (char *one = codes; one != NULL && n < 8;) {
        char *rest = strchr(one, '+');
        if (rest != NULL) {
            *rest++ = '\0';
        }
        cheat_code c;
        found[n++] = cheats_decode(one, &c) ? cheats_find(&c) : -1;
        one = rest;
    }
    return n;
}

int fe_gg_db_state(int index)
{
    int found[8];
    int n = db_codes_in_list(index, found);
    bool all_in = n > 0, all_on = n > 0;
    for (int i = 0; i < n; i++) {
        all_in = all_in && found[i] >= 0;
        all_on = all_on && found[i] >= 0 && cheats_get(found[i])->on;
    }
    return all_on ? 2 : (all_in ? 1 : 0);
}

void fe_gg_db_set(int index, bool on)
{
    if (on) {
        cheats_db_add(index);
        return;
    }
    int found[8];
    int n = db_codes_in_list(index, found);
    for (int i = 0; i < n; i++) {
        if (found[i] >= 0) {
            cheats_set_on(found[i], false);
        }
    }
}
