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
#include "log.h"
#include <stdio.h>
#include <inttypes.h>
#include "settings.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_LEVEL LOG_LEVEL_INFO

uint8_t log_levels[LOG_CATEGORIES] = {
    DEFAULT_LEVEL, DEFAULT_LEVEL, DEFAULT_LEVEL, DEFAULT_LEVEL, DEFAULT_LEVEL, DEFAULT_LEVEL,
    DEFAULT_LEVEL, DEFAULT_LEVEL, DEFAULT_LEVEL, DEFAULT_LEVEL, DEFAULT_LEVEL, DEFAULT_LEVEL,
};

static const char *const level_names[] = {"off", "error", "warn", "info", "debug", "trace"};
static const char *const category_names[LOG_CATEGORIES] = {
    "emu", "cpu", "ppu", "apu", "dma", "mapper", "input", "video", "audio", "savestate", "cfg", "script",
};
static const char *const category_descriptions[LOG_CATEGORIES] = {
    "the program: start, stop, files",
    "the 6502: interrupts, unusual instructions",
    "the picture processor",
    "the sound processor",
    "OAM and DMC transfers",
    "cartridges, iNES files, battery saves",
    "keyboard and gamepads",
    "the window",
    "the sound output",
    "save states",
    "settings files",
    "Lua scripts: print() and errors",
};

static FILE *log_file = NULL; // NULL: stderr, or stdout while the trace is on stderr
static char log_path[1024];   // the file's name when log_file came from log_set_file_path(), else ""
static bool trace_on_stderr = false;
static bool console = true;
static log_hook hook = NULL;
static void *hook_context = NULL;
static log_clock clock_fn = NULL;

int log_level_from_name(const char *name)
{
    for (int i = 0; i < (int)(sizeof(level_names) / sizeof(level_names[0])); i++) {
        if (strcmp(name, level_names[i]) == 0) {
            return i;
        }
    }
    if (strcmp(name, "warning") == 0) {
        return LOG_LEVEL_WARN;
    }
    return -1;
}

int log_category_from_name(const char *name)
{
    for (int i = 0; i < LOG_CATEGORIES; i++) {
        if (strcmp(name, category_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

const char *log_level_name(log_level level)
{
    return level_names[level];
}

const char *log_category_name(log_category category)
{
    return category_names[category];
}

void log_set_file(FILE *file)
{
    log_file = file;
    log_path[0] = '\0';
}

bool log_set_file_path(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return false;
    }
    setvbuf(f, NULL, _IOLBF, 4096);
    log_file = f;
    snprintf(log_path, sizeof(log_path), "%s", path);
    return true;
}

const char *log_destination(void)
{
    if (log_path[0] != '\0') {
        return log_path;
    }
    return log_file != NULL ? "a file" : (trace_on_stderr ? "stdout" : "stderr");
}

void log_set_trace_on_stderr(bool on)
{
    trace_on_stderr = on;
}

void log_set_console(bool on)
{
    console = on;
}

void log_set_hook(log_hook fn, void *context)
{
    hook = fn;
    hook_context = context;
}

void log_set_clock(log_clock fn)
{
    clock_fn = fn;
}

void log_write(log_category category, log_level level, const char *format, ...)
{
    char line[1400];
    size_t used = 0;
    if (level >= LOG_LEVEL_DEBUG) {
        if (clock_fn != NULL) {
            uint32_t frame = 0;
            uint64_t cycle = 0;
            int scanline = 0;
            int dot = 0;
            clock_fn(&frame, &cycle, &scanline, &dot);
            used = (size_t)snprintf(line, sizeof(line), "[f=%u c=%" PRIu64 " %d,%d] ", frame, cycle, scanline, dot);
        }
        used += (size_t)snprintf(line + used, sizeof(line) - used, "%s: ", category_names[category]);
    } else if (level == LOG_LEVEL_WARN) {
        used = (size_t)snprintf(line, sizeof(line), "warning: ");
    } else if (level == LOG_LEVEL_ERROR) {
        used = (size_t)snprintf(line, sizeof(line), "error: ");
    }
    va_list args;
    va_start(args, format);
    vsnprintf(line + used, sizeof(line) - used, format, args);
    va_end(args);
    size_t len = strlen(line);
    while (len > 0 && line[len - 1] == '\n') { // (messages carry no newline, but some old ones did)
        line[--len] = '\0';
    }

    if (console) {
        FILE *out = log_file != NULL ? log_file : (trace_on_stderr ? stdout : stderr);
        fprintf(out, "%s\n", line);
    }
    if (hook != NULL) {
        hook(category, level, line, hook_context);
    }
}

// "debug,ppu=trace" or "ppu=debug apu=trace"
bool log_set_levels(const char *spec, char *error, size_t error_size)
{
    uint8_t levels[LOG_CATEGORIES];
    memcpy(levels, log_levels, sizeof(levels));
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", spec);
    for (char *item = strtok(copy, ", \t"); item != NULL; item = strtok(NULL, ", \t")) {
        for (char *c = item; *c != '\0'; c++) {
            *c = (char)tolower((unsigned char)*c);
        }
        char *eq = strchr(item, '=');
        const char *level_text = eq ? eq + 1 : item;
        int level = log_level_from_name(level_text);
        if (level < 0) {
            snprintf(error, error_size, "unknown log level '%s' (off, error, warn, info, debug, trace)", level_text);
            return false;
        }
        if (eq == NULL || strncmp(item, "all=", 4) == 0) {
            memset(levels, level, sizeof(levels));
            continue;
        }
        *eq = '\0';
        int category = log_category_from_name(item);
        if (category < 0) {
            snprintf(error, error_size, "unknown log category '%s'", item);
            return false;
        }
        levels[category] = (uint8_t)level;
    }
    memcpy(log_levels, levels, sizeof(levels));
    return true;
}

// --- settings (settings.h): log.level and log.<category> ------------------------------------------------------

static int level_all = DEFAULT_LEVEL;
static int level_of[LOG_CATEGORIES] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}; // -1: as log.level

static bool apply_log(const setting *s, const char *value, char *error, size_t error_size)
{
    if (error_size > 0) { // (never fails)
        error[0] = '\0';
    }
    int level = value[0] != '\0' ? log_level_from_name(value) : -1;
    if (s->id < 0) {
        level_all = level;
    } else {
        level_of[s->id] = level;
    }
    for (int i = 0; i < LOG_CATEGORIES; i++) {
        log_levels[i] = (uint8_t)(level_of[i] >= 0 ? level_of[i] : level_all);
    }
    return true;
}

static const char *const level_choices[] = {"off", "error", "warn", "info", "debug", "trace", NULL};
static const char *const category_choices[] = {"", "off", "error", "warn", "info", "debug", "trace", NULL};

#define LOG_CATEGORY_SETTING(cat, n)                                                                                   \
    {                                                                                                                  \
        "log." cat, SETTING_CHOICE, "", cat, NULL, NULL, 0, 0, category_choices, 0, apply_log, n                       \
    }

static setting log_table[] = {
    {"log.level", SETTING_CHOICE, "info", "All parts", NULL, NULL, 0, 0, level_choices, 0, apply_log, -1},
    LOG_CATEGORY_SETTING("emu", 0),
    LOG_CATEGORY_SETTING("cpu", 1),
    LOG_CATEGORY_SETTING("ppu", 2),
    LOG_CATEGORY_SETTING("apu", 3),
    LOG_CATEGORY_SETTING("dma", 4),
    LOG_CATEGORY_SETTING("mapper", 5),
    LOG_CATEGORY_SETTING("input", 6),
    LOG_CATEGORY_SETTING("video", 7),
    LOG_CATEGORY_SETTING("audio", 8),
    LOG_CATEGORY_SETTING("savestate", 9),
    LOG_CATEGORY_SETTING("cfg", 10),
    LOG_CATEGORY_SETTING("script", 11),
};

const setting_group *log_settings(void)
{
    static bool described = false;
    if (!described) { // the categories' descriptions and menu sections come from the tables above
        log_table[0].section = "Messages up to this level";
        log_table[1].section = "By part (empty: as above)";
        for (int i = 0; i < LOG_CATEGORIES; i++) {
            log_table[1 + i].description = category_descriptions[i];
        }
        described = true;
    }
    static const setting_group group = {
        "messages", "Logging",
        "How much each part of the emulator tells: off, error, warn, info, debug or trace (debug and trace\n"
        "messages start with the emulation time: [f=frame c=CPU cycle scanline,dot]). log.level sets them\n"
        "all; log.CATEGORY (empty = as log.level) one of them. --log-level on the command line overrides.",
        log_table, (int)(sizeof(log_table) / sizeof(log_table[0]))};
    return &group;
}
