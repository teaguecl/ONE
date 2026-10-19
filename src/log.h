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
// Console logging for the emulator: everything that is not the Mesen-format trace (trace.c, its own stream) and not
// output the user asked for (--help, --list-gamepads, -D: plain printf to stdout).
//
//   LOG_ERROR(MAPPER, "mapper %d is not supported", n);
//   LOG_WARN(CFG, "%s:%d: unknown setting '%s'", path, line, key);
//   LOG_INFO(SAVESTATE, "Saved to slot %d", slot);
//   LOG_DEBUG(PPU, "sprite 0 hit at %d,%d", x, y);
//   LOG_TRACE(APU, "frame counter step %d", step);
//
// Each category has its own level (error, warn, info, debug, trace; off). A message below its category's level
// costs one comparison: its arguments are not even evaluated. Defaults: info for every category. Settings:
// one.cfg "log.level = debug" (every category), "log.ppu = trace" (one category); command line
// --log-level debug,ppu=trace; the debugger's "log" command. Building with -DONE_LOG_MAX_LEVEL=LOG_LEVEL_INFO
// removes the debug and trace messages from the program altogether.
//
// Where: stderr (or --log FILE), except while the trace (-t) is written to stderr: then stdout, so the trace stays
// clean. Debug and trace messages start with the emulation time, "[f=FRAME c=CPU_CYCLE SCANLINE,DOT]";
// warnings and errors with "warning:" / "error:". A hook (log_set_hook) sees every message too: the debugger shows
// them in its command window.
#ifndef log_h_defined
#define log_h_defined

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "settings.h"

typedef enum {
    LOG_LEVEL_OFF = 0,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_WARN,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_TRACE,
} log_level;

// The categories: one per part of the emulator. (Names in settings: lower case, as in log_category_name().)
typedef enum {
    LOG_CAT_EMU, // the program: start, stop, files
    LOG_CAT_CPU,
    LOG_CAT_PPU,
    LOG_CAT_APU,
    LOG_CAT_DMA,
    LOG_CAT_MAPPER, // cartridges, iNES files, .sav files
    LOG_CAT_INPUT,
    LOG_CAT_VIDEO,
    LOG_CAT_AUDIO,
    LOG_CAT_SAVESTATE,
    LOG_CAT_CFG,    // settings files
    LOG_CAT_SCRIPT, // Lua scripts: their print() / emu.log() output and errors
    LOG_CATEGORIES
} log_category;

#ifndef ONE_LOG_MAX_LEVEL
#define ONE_LOG_MAX_LEVEL LOG_LEVEL_TRACE
#endif

extern uint8_t log_levels[LOG_CATEGORIES];

void log_write(log_category category, log_level level, const char *format, ...)
#if defined(__MINGW_PRINTF_FORMAT)
    __attribute__((format(__MINGW_PRINTF_FORMAT, 3, 4))) // (MinGW: printf with %zu, as __USE_MINGW_ANSI_STDIO makes it)
#elif defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#define LOG_ENABLED(cat, lvl) ((lvl) <= ONE_LOG_MAX_LEVEL && log_levels[LOG_CAT_##cat] >= (lvl))
#define LOG_AT(cat, lvl, ...)                                                                                          \
    do {                                                                                                               \
        if (LOG_ENABLED(cat, lvl)) {                                                                                   \
            log_write(LOG_CAT_##cat, (lvl), __VA_ARGS__);                                                              \
        }                                                                                                              \
    } while (0)
#define LOG_ERROR(cat, ...) LOG_AT(cat, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_WARN(cat, ...)  LOG_AT(cat, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_INFO(cat, ...)  LOG_AT(cat, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_DEBUG(cat, ...) LOG_AT(cat, LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_TRACE(cat, ...) LOG_AT(cat, LOG_LEVEL_TRACE, __VA_ARGS__)

// Only the first time this line is reached (for events that would otherwise repeat every frame or every write)
#define LOG_ONCE(cat, lvl, ...)                                                                                        \
    do {                                                                                                               \
        static bool log_once_done_;                                                                                    \
        if (!log_once_done_ && LOG_ENABLED(cat, lvl)) {                                                                \
            log_once_done_ = true;                                                                                     \
            log_write(LOG_CAT_##cat, (lvl), __VA_ARGS__);                                                              \
        }                                                                                                              \
    } while (0)

// Levels and categories by name ("debug", "ppu"): -1 if unknown
int log_level_from_name(const char *name);
int log_category_from_name(const char *name);
const char *log_level_name(log_level level);
const char *log_category_name(log_category category);

// "info", "debug,ppu=trace", "ppu=debug apu=trace": set levels. Returns false (with a message in ERROR) on a
// mistake, and then changes nothing.
bool log_set_levels(const char *spec, char *error, size_t error_size);

// one.cfg: log.level and log.<category> (settings.h)
const setting_group *log_settings(void);

// Where messages go: FILE (NULL: the default, stderr / stdout as above). log_set_file_path opens a file.
void log_set_file(FILE *file);
bool log_set_file_path(const char *path);
const char *log_destination(void);     // where messages go now, for the menu: the file's name, "stderr" or "stdout"
void log_set_trace_on_stderr(bool on); // the trace is written to stderr: messages go to stdout meanwhile
void log_set_console(bool on);         // false: only the hook sees messages (the debugger before its log file opens)

// The hook sees every message that is written, already formatted (without the trailing newline)
typedef void (*log_hook)(log_category category, log_level level, const char *message, void *context);
void log_set_hook(log_hook fn, void *context);

// Where the emulation is, for the time stamps of debug and trace messages (set by emu.c; NULL: no time stamps)
typedef void (*log_clock)(uint32_t *frame, uint64_t *cpu_cycle, int *scanline, int *dot);
void log_set_clock(log_clock fn);

#endif
