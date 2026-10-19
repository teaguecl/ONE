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
#include "settings.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef _WIN32
#include <direct.h> // _mkdir
#endif
#include "audio.h"
#include "cfgfile.h"
#include "input.h"
#include "log.h"
#include "mappers/mapper_txrom.h"
#include "menu_hint.h"
#include "video.h"

// The groups, in one.cfg order
typedef const setting_group *(*group_fn)(void);
static const group_fn group_fns[] = {input_controller_settings,
                                     input_hotkey_settings,
                                     menu_hint_settings, // (menu_hint.c)
                                     video_settings,
                                     audio_settings,
                                     log_settings,
                                     mapper_txrom_settings}; // (mapper_txrom.c)
#define GROUPS       ((int)(sizeof(group_fns) / sizeof(group_fns[0])))
#define MAX_SETTINGS 128

static char *values[MAX_SETTINGS]; // the current value of every setting, by settings_index()
static bool changed[MAX_SETTINGS]; // changed since the last load or save
static char loaded_path[512];      // the file settings_load() read ("" = none)

int settings_group_count(void)
{
    return GROUPS;
}

const setting_group *settings_group(int index)
{
    return (index >= 0 && index < GROUPS) ? group_fns[index]() : NULL;
}

// The number of S in the whole table (all groups one after the other), -1 if S is not in it
static int settings_index(const setting *s)
{
    int base = 0;
    for (int g = 0; g < GROUPS; g++) {
        const setting_group *group = group_fns[g]();
        if (s >= group->settings && s < group->settings + group->count) {
            return base + (int)(s - group->settings);
        }
        base += group->count;
    }
    return -1;
}

const setting *settings_find(const char *key)
{
    for (int g = 0; g < GROUPS; g++) {
        const setting_group *group = group_fns[g]();
        for (int i = 0; i < group->count; i++) {
            if (strcmp(group->settings[i].key, key) == 0) {
                return &group->settings[i];
            }
        }
    }
    return NULL;
}

bool settings_parse_bool(const char *text, bool *out)
{
    static const char *const yes[] = {"on", "yes", "true", "1"};
    static const char *const no[] = {"off", "no", "false", "0"};
    for (int i = 0; i < 4; i++) {
        if (strcmp(text, yes[i]) == 0) {
            *out = true;
            return true;
        }
        if (strcmp(text, no[i]) == 0) {
            *out = false;
            return true;
        }
    }
    return false;
}

// The checks every setting of a type gets before its apply function sees the value
static bool check_type(const setting *s, const char *value, char *error, size_t error_size)
{
    switch (s->type) {
    case SETTING_BOOL: {
        bool b = false;
        if (!settings_parse_bool(value, &b)) {
            snprintf(error, error_size, "expected on or off, not '%s'", value);
            return false;
        }
        return true;
    }
    case SETTING_INT: {
        char *end = NULL;
        errno = 0;
        long n = strtol(value, &end, 10);
        if (end == value || *end != '\0' || errno != 0 || n < s->min || n > s->max) {
            snprintf(error, error_size, "expected a number %d-%d, not '%s'", s->min, s->max, value);
            return false;
        }
        return true;
    }
    case SETTING_CHOICE: {
        size_t used = (size_t)snprintf(error, error_size, "expected");
        for (const char *const *c = s->choices; *c != NULL; c++) {
            if (strcmp(*c, value) == 0) {
                return true;
            }
            if (used < error_size) {
                used += (size_t)snprintf(error + used, error_size - used, "%s %s", c == s->choices ? "" : ",",
                                         (*c)[0] ? *c : "(empty)");
            }
        }
        if (used < error_size) {
            snprintf(error + used, error_size - used, ", not '%s'", value);
        }
        return false;
    }
    default: return true;
    }
}

static bool apply(const setting *s, const char *value, char *error, size_t error_size)
{
    if (!check_type(s, value, error, error_size)) {
        return false;
    }
    if (s->apply != NULL && !s->apply(s, value, error, error_size)) {
        return false;
    }
    int index = settings_index(s);
    if (index >= 0) {
        char *copy = malloc(strlen(value) + 1);
        if (copy != NULL) {
            strcpy(copy, value); // NOLINT(clang-analyzer-security.insecureAPI.strcpy): sized above
            free(values[index]);
            values[index] = copy;
        }
    }
    return true;
}

const char *settings_value(const setting *s)
{
    int index = settings_index(s);
    if (index < 0) {
        return "";
    }
    return values[index] != NULL ? values[index] : s->default_value;
}

const char *settings_get(const char *key)
{
    const setting *s = settings_find(key);
    return s != NULL ? settings_value(s) : NULL;
}

bool settings_set(const setting *s, const char *value, char *error, size_t error_size)
{
    if (strcmp(settings_value(s), value) == 0) {
        return true;
    }
    if (!apply(s, value, error, error_size)) {
        return false;
    }
    changed[settings_index(s)] = true;
    LOG_DEBUG(CFG, "%s = %s", s->key, value);
    return true;
}

bool settings_set_key(const char *key, const char *value, char *error, size_t error_size)
{
    const setting *s = settings_find(key);
    if (s == NULL) {
        snprintf(error, error_size, "unknown setting '%s'", key);
        return false;
    }
    return settings_set(s, value, error, error_size);
}

bool settings_changed(void)
{
    for (int i = 0; i < MAX_SETTINGS; i++) {
        if (changed[i]) {
            return true;
        }
    }
    return false;
}

int settings_load_defaults(void)
{
    loaded_path[0] = '\0';
    for (int g = 0; g < GROUPS; g++) {
        const setting_group *group = group_fns[g]();
        for (int i = 0; i < group->count; i++) {
            const setting *s = &group->settings[i];
            char error[300];
            if (!apply(s, s->default_value, error, sizeof(error))) {
                LOG_ERROR(CFG, "%s: the default '%s' does not work: %s", s->key, s->default_value, error);
            }
            changed[settings_index(s)] = false;
        }
    }
    return 0;
}

int settings_load(const char *path)
{
    char found[512];
    if (path == NULL && cfg_find_default(found, sizeof(found))) {
        path = found;
    }
    cfg_file cfg;
    bool have_file = false;
    if (path != NULL) {
        if (!cfg_load(&cfg, path)) {
            LOG_ERROR(CFG, "cannot open settings file '%s'", path);
            return -1;
        }
        have_file = true;
        LOG_INFO(CFG, "Settings from %s", path);
    }
    snprintf(loaded_path, sizeof(loaded_path), "%s", have_file ? path : "");
    int errors = 0;
    for (int g = 0; g < GROUPS; g++) {
        const setting_group *group = group_fns[g]();
        for (int i = 0; i < group->count; i++) {
            const setting *s = &group->settings[i];
            const char *value = have_file ? cfg_get(&cfg, s->key) : NULL;
            char error[300];
            if (value != NULL && !apply(s, value, error, sizeof(error))) {
                LOG_WARN(CFG, "%s:%d: %s: %s", cfg.path, cfg_line(&cfg, s->key), s->key, error);
                errors++;
                value = NULL;
            }
            if (value == NULL && !apply(s, s->default_value, error, sizeof(error))) {
                LOG_ERROR(CFG, "%s: the default '%s' does not work: %s", s->key, s->default_value, error);
            }
            changed[settings_index(s)] = false;
        }
    }
    if (have_file) {
        errors += cfg_report_unused(&cfg);
        cfg_free(&cfg);
    }
    return errors;
}

// --- writing ----------------------------------------------------------------------------------------------------

// "# " + each line of TEXT
static void write_comment_lines(FILE *out, const char *text)
{
    while (text != NULL && *text != '\0') {
        const char *nl = strchr(text, '\n');
        int len = nl ? (int)(nl - text) : (int)strlen(text);
        fprintf(out, "#%s%.*s\n", len > 0 ? " " : "", len, text);
        text = nl ? nl + 1 : NULL;
    }
}

void settings_write_defaults(FILE *out)
{
    fprintf(out,
            "# ONE emulator settings (one.cfg).  `one --write-config FILE` writes this file with every setting at\n"
            "# its default.  One \"key = value\" per line; '#' starts a comment.  `one` reads --config FILE, else\n"
            "# ./one.cfg, else ~/.config/one/one.cfg ($XDG_CONFIG_HOME/one/one.cfg); one-debugger reads the same\n"
            "# file (--one-config FILE).  Settings that are left out keep their defaults.  The menu of the game\n"
            "# window (Esc) changes them too, and saves its changes into the file it read.\n");
    for (int g = 0; g < GROUPS; g++) {
        const setting_group *group = group_fns[g]();
        fprintf(out, "\n# --- %s ---\n", group->name);
        write_comment_lines(out, group->intro);
        // the comments of a run of settings (up to an empty line) start in one column
        int start = 0;
        while (start < group->count) {
            int end = start + 1;
            while (end < group->count && !(group->settings[end].flags & SETTING_GAP_ABOVE)) {
                end++;
            }
            int column = 0;
            int key_width = 0; // a run without comments lines up its '=' signs instead
            bool comments = false;
            for (int i = start; i < end; i++) {
                int len = (int)(strlen(group->settings[i].key) + 3 + strlen(group->settings[i].default_value));
                column = len > column ? len : column;
                int key_len = (int)strlen(group->settings[i].key);
                key_width = key_len > key_width ? key_len : key_width;
                comments = comments || group->settings[i].description != NULL;
            }
            column += 2;
            if (comments) {
                key_width = 0;
            }
            if (start > 0) {
                fprintf(out, "\n");
            }
            for (int i = start; i < end; i++) {
                const setting *s = &group->settings[i];
                char line[512];
                snprintf(line, sizeof(line), "%-*s = %s", key_width, s->key, s->default_value);
                if (s->description == NULL) {
                    fprintf(out, "%s\n", line);
                    continue;
                }
                const char *text = s->description;
                bool first = true;
                while (text != NULL) {
                    const char *nl = strchr(text, '\n');
                    int len = nl ? (int)(nl - text) : (int)strlen(text);
                    fprintf(out, "%-*s# %.*s\n", column, first ? line : "", len, text);
                    first = false;
                    text = nl ? nl + 1 : NULL;
                }
            }
            start = end;
        }
    }
}

// --- saving -----------------------------------------------------------------------------------------------------

bool settings_user_path(char *buf, size_t size)
{
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata != NULL && *appdata != '\0') {
        snprintf(buf, size, "%s\\one\\one.cfg", appdata);
        return true;
    }
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg != NULL && *xdg != '\0') {
        snprintf(buf, size, "%s/one/one.cfg", xdg);
        return true;
    }
    if (home != NULL && *home != '\0') {
        snprintf(buf, size, "%s/.config/one/one.cfg", home);
        return true;
    }
#endif
    snprintf(buf, size, "one.cfg");
    return true;
}

void settings_user_dir(char *buf, size_t size)
{
    settings_user_path(buf, size);
    char *end = strrchr(buf, '/');
#ifdef _WIN32
    char *back = strrchr(buf, '\\');
    if (back != NULL && (end == NULL || back > end)) {
        end = back;
    }
#endif
    if (end != NULL) {
        *end = '\0';
    } else {
        snprintf(buf, size, ".");
    }
}

const char *settings_path(void)
{
    static char path[512];
    if (loaded_path[0] != '\0') {
        return loaded_path;
    }
    settings_user_path(path, sizeof(path));
    return path;
}

// Create the directories of PATH's directory part (mkdir -p)
void settings_make_parent_dirs(const char *path)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    for (char *p = dir + 1; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
#ifdef _WIN32
            _mkdir(dir);
#else
            mkdir(dir, 0755);
#endif
            *p = c;
        }
    }
}

// The key a line sets ("  key = value # x" -> key), or false if it is a comment or blank
static bool line_key(const char *line, char *key, size_t key_size)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (*line == '#' || *line == '\0' || *line == '\n') {
        return false;
    }
    const char *eq = strchr(line, '=');
    if (eq == NULL) {
        return false;
    }
    const char *end = eq;
    while (end > line && isspace((unsigned char)end[-1])) {
        end--;
    }
    snprintf(key, key_size, "%.*s", (int)(end - line), line);
    return true;
}

// LINE with its value replaced by VALUE: the key and a comment after the value stay where they are
static void replace_value(const char *line, const char *value, char *out, size_t out_size)
{
    const char *eq = strchr(line, '=');
    const char *v = eq + 1;
    while (*v == ' ' || *v == '\t') {
        v++;
    }
    const char *comment = NULL;
    for (const char *c = v; *c != '\0' && *c != '\n'; c++) {
        if (*c == '#' && (c == v || isspace((unsigned char)c[-1]))) {
            comment = c;
            break;
        }
    }
    int head = (int)(eq + 1 - line);
    int used = snprintf(out, out_size, "%.*s %s", head, line, value);
    if (comment != NULL && used >= 0 && (size_t)used < out_size) {
        int column = (int)(comment - line);
        int pad = column > used + 1 ? column - used : 2;
        int len = (int)strcspn(comment, "\n");
        snprintf(out + used, out_size - (size_t)used, "%*s%.*s", pad, "", len, comment);
    }
}

// A line in $XDG_STATE_HOME/one/settings-saves.log (~/.local/state/one/) for every setting settings_save() writes: when,
// which process, from where. Settings that change by themselves are hard to explain without it.
static void audit_save(const char *key, const char *value)
{
#ifndef _WIN32
    const char *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    char path[600];
    if (state != NULL && *state != '\0') {
        snprintf(path, sizeof(path), "%s/one/settings-saves.log", state);
    } else if (home != NULL && *home != '\0') {
        snprintf(path, sizeof(path), "%s/.local/state/one/settings-saves.log", home);
    } else {
        return;
    }
    FILE *f = fopen(path, "a");
    if (f == NULL) {
        return;
    }
    char cwd[256] = "?";
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        snprintf(cwd, sizeof(cwd), "?");
    }
    time_t now = time(NULL);
    char when[32];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "%s pid %d cwd %s: %s = %s\n", when, (int)getpid(), cwd, key, value);
    fclose(f);
#else
    (void)key, (void)value;
#endif
}

bool settings_save(char *error, size_t error_size)
{
    char path[512]; // (a copy: settings_path() may be loaded_path itself, which is rewritten below)
    snprintf(path, sizeof(path), "%s", settings_path());
    FILE *in = fopen(path, "r");
    if (in == NULL) {
        // no file yet: one with every default, then the changes go into it
        settings_make_parent_dirs(path);
        FILE *out = fopen(path, "w");
        if (out == NULL) {
            snprintf(error, error_size, "cannot write %s", path);
            return false;
        }
        settings_write_defaults(out);
        fclose(out);
        in = fopen(path, "r");
        if (in == NULL) {
            snprintf(error, error_size, "cannot read %s", path);
            return false;
        }
        snprintf(loaded_path, sizeof(loaded_path), "%s", path);
    }
    // the file's lines
    char **lines = NULL;
    int count = 0;
    int capacity = 0;
    char text[1024];
    while (fgets(text, sizeof(text), in) != NULL) {
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 128;
            char **grown = (char **)realloc((void *)lines, (size_t)capacity * sizeof(char *));
            if (grown == NULL) {
                break;
            }
            lines = grown;
        }
        text[strcspn(text, "\n")] = '\0';
        lines[count] = malloc(strlen(text) + 1);
        if (lines[count] != NULL) {
            strcpy(lines[count], text); // NOLINT(clang-analyzer-security.insecureAPI.strcpy): sized above
            count++;
        }
    }
    fclose(in);

    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *out = fopen(tmp, "w");
    if (out == NULL) {
        snprintf(error, error_size, "cannot write %s", tmp);
        for (int i = 0; i < count; i++) {
            free(lines[i]);
        }
        free((void *)lines);
        return false;
    }
    // which line sets each changed setting (the last one: that is the one that counts)
    int base = 0;
    bool added_heading = false;
    char appended[MAX_SETTINGS][300];
    int appended_count = 0;
    for (int g = 0; g < GROUPS; g++) {
        const setting_group *group = group_fns[g]();
        for (int i = 0; i < group->count; i++, base++) {
            if (!changed[base]) {
                continue;
            }
            const setting *s = &group->settings[i];
            audit_save(s->key, settings_value(s));
            int at = -1;
            char key[128];
            for (int l = 0; l < count; l++) {
                if (line_key(lines[l], key, sizeof(key)) && strcmp(key, s->key) == 0) {
                    at = l;
                }
            }
            if (at >= 0) {
                char replaced[1100];
                replace_value(lines[at], settings_value(s), replaced, sizeof(replaced));
                char *copy = malloc(strlen(replaced) + 1);
                if (copy != NULL) {
                    strcpy(copy, replaced); // NOLINT(clang-analyzer-security.insecureAPI.strcpy): sized above
                    free(lines[at]);
                    lines[at] = copy;
                }
            } else if (appended_count < MAX_SETTINGS) {
                snprintf(appended[appended_count++], sizeof(appended[0]), "%s = %s", s->key, settings_value(s));
            }
        }
    }
    for (int l = 0; l < count; l++) {
        fprintf(out, "%s\n", lines[l]);
        free(lines[l]);
    }
    free((void *)lines);
    for (int i = 0; i < appended_count; i++) {
        if (!added_heading) {
            fprintf(out, "\n# --- added by the menu ---\n");
            added_heading = true;
        }
        fprintf(out, "%s\n", appended[i]);
    }
    if (fclose(out) != 0) {
        snprintf(error, error_size, "cannot write %s", tmp);
        remove(tmp);
        return false;
    }
#ifdef _WIN32
    remove(path); // (rename() does not replace a file on Windows)
#endif
    if (rename(tmp, path) != 0) {
        snprintf(error, error_size, "cannot replace %s", path);
        remove(tmp);
        return false;
    }
    memset(changed, 0, sizeof(changed));
    LOG_INFO(CFG, "Settings saved to %s", path);
    return true;
}
