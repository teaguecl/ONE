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
#include "cfgfile.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"

static char *dup_range(const char *start, const char *end)
{
    size_t n = (size_t)(end - start);
    char *s = malloc(n + 1);
    if (s != NULL) {
        memcpy(s, start, n);
        s[n] = '\0';
    }
    return s;
}

static const char *skip_space(const char *p)
{
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static const char *trim_end(const char *start, const char *end)
{
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    return end;
}

bool cfg_load(cfg_file *cfg, const char *path)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->path, sizeof(cfg->path), "%s", path);
    FILE *in = fopen(path, "r");
    if (in == NULL) {
        return false;
    }
    char text[1024];
    int line = 0;
    int capacity = 0;
    while (fgets(text, sizeof(text), in) != NULL) {
        line++;
        const char *p = skip_space(text);
        if (*p == '\0' || *p == '#') {
            continue;
        }
        const char *eq = strchr(p, '=');
        if (eq == NULL) {
            LOG_WARN(CFG, "%s:%d: expected 'key = value'", path, line);
            continue;
        }
        const char *key_end = trim_end(p, eq);
        const char *value = skip_space(eq + 1);
        // a comment after the value: '#' at the start of a word ("... # comment")
        const char *value_end = value + strlen(value);
        for (const char *c = value; *c != '\0'; c++) {
            if (*c == '#' && (c == value || isspace((unsigned char)c[-1]))) {
                value_end = c;
                break;
            }
        }
        value_end = trim_end(value, value_end);
        if (key_end == p) {
            LOG_WARN(CFG, "%s:%d: missing key before '='", path, line);
            continue;
        }
        if (cfg->count == capacity) {
            capacity = capacity ? capacity * 2 : 32;
            cfg_entry *grown = realloc(cfg->entries, (size_t)capacity * sizeof(cfg_entry));
            if (grown == NULL) {
                break;
            }
            cfg->entries = grown;
        }
        cfg_entry *e = &cfg->entries[cfg->count];
        e->key = dup_range(p, key_end);
        e->value = dup_range(value, value_end);
        e->line = line;
        e->used = false;
        if (e->key != NULL && e->value != NULL) {
            cfg->count++;
        } else {
            free(e->key);
            free(e->value);
        }
    }
    fclose(in);
    return true;
}

void cfg_free(cfg_file *cfg)
{
    for (int i = 0; i < cfg->count; i++) {
        free(cfg->entries[i].key);
        free(cfg->entries[i].value);
    }
    free(cfg->entries);
    cfg->entries = NULL;
    cfg->count = 0;
}

static cfg_entry *find(const cfg_file *cfg, const char *key)
{
    cfg_entry *found = NULL;
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0) {
            found = &cfg->entries[i]; // the last one wins
        }
    }
    return found;
}

const char *cfg_get(cfg_file *cfg, const char *key)
{
    const char *value = NULL;
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0) {
            cfg->entries[i].used = true;
            value = cfg->entries[i].value;
        }
    }
    return value;
}

int cfg_line(const cfg_file *cfg, const char *key)
{
    const cfg_entry *e = find(cfg, key);
    return e ? e->line : 0;
}

int cfg_report_unused(const cfg_file *cfg)
{
    int count = 0;
    for (int i = 0; i < cfg->count; i++) {
        if (!cfg->entries[i].used) {
            LOG_WARN(CFG, "%s:%d: unknown setting '%s'", cfg->path, cfg->entries[i].line, cfg->entries[i].key);
            count++;
        }
    }
    return count;
}

static bool exists(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    fclose(f);
    return true;
}

bool cfg_find_default(char *buf, size_t size)
{
    if (exists("one.cfg")) {
        snprintf(buf, size, "%s", "one.cfg");
        return true;
    }
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata != NULL && *appdata != '\0') {
        snprintf(buf, size, "%s\\one\\one.cfg", appdata);
        if (exists(buf)) {
            return true;
        }
    }
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg != NULL && *xdg != '\0') {
        snprintf(buf, size, "%s/one/one.cfg", xdg);
        if (exists(buf)) {
            return true;
        }
    }
    if (home != NULL && *home != '\0') {
        snprintf(buf, size, "%s/.config/one/one.cfg", home);
        if (exists(buf)) {
            return true;
        }
    }
#endif
    buf[0] = '\0';
    return false;
}
