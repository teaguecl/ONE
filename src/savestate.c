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
#include "savestate.h"
#include "cheats.h"
#include "crc32.h"
#include "script.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "cpu.h"
#include "libco/libco.h"
#include "ppu.h"
#include "rp2a03.h"
#include "video.h"
#include "zipfile.h"

#define FORMAT_VERSION 3
#define MAGIC          "ONESTATE"
#define SLOTS          9

extern cothread_t ppu_thread;

// --- reading and writing variables ---------------------------------------------------------------------------

typedef enum { MODE_SAVE, MODE_CHECK, MODE_APPLY } state_mode;

typedef struct {
    char name[96];
    size_t offset; // of the data in the file
    uint32_t size;
    bool used;
} field;

struct state_io {
    state_mode mode;
    const char *section;
    // MODE_SAVE: the file being built
    uint8_t *out;
    size_t out_size, out_capacity;
    uint32_t count;
    // MODE_CHECK / MODE_APPLY: the file being loaded
    const uint8_t *in;
    field *fields;
    uint32_t field_count;
    char error[256]; // the first problem found by MODE_CHECK
};

static void out_bytes(state_io *io, const void *data, size_t size)
{
    if (io->out_size + size > io->out_capacity) {
        size_t capacity = io->out_capacity ? io->out_capacity : 65536;
        while (capacity < io->out_size + size) {
            capacity *= 2;
        }
        uint8_t *grown = realloc(io->out, capacity);
        if (grown == NULL) {
            snprintf(io->error, sizeof(io->error), "out of memory");
            return;
        }
        io->out = grown;
        io->out_capacity = capacity;
    }
    memcpy(io->out + io->out_size, data, size);
    io->out_size += size;
}

static void out_u32(state_io *io, uint32_t value)
{
    uint8_t b[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    out_bytes(io, b, 4);
}

static uint32_t in_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool state_loading(const state_io *io)
{
    return io->mode == MODE_APPLY;
}

void state_bytes(state_io *io, const char *name, void *data, size_t size)
{
    char full[96];
    snprintf(full, sizeof(full), "%s.%s", io->section, name);
    if (io->mode == MODE_SAVE) {
        uint16_t len = (uint16_t)strlen(full);
        uint8_t l[2] = {(uint8_t)len, (uint8_t)(len >> 8)};
        out_bytes(io, l, 2);
        out_bytes(io, full, len);
        out_u32(io, (uint32_t)size);
        out_bytes(io, data, size);
        io->count++;
        return;
    }
    for (uint32_t i = 0; i < io->field_count; i++) {
        field *f = &io->fields[i];
        if (strcmp(f->name, full) == 0) {
            if (f->size != size) {
                if (io->error[0] == '\0') {
                    snprintf(io->error, sizeof(io->error), "%s has %u bytes, this version has %zu", full,
                             (unsigned)f->size, size);
                }
            } else if (io->mode == MODE_APPLY) {
                memcpy(data, io->in + f->offset, size);
            }
            f->used = true;
            return;
        }
    }
    if (io->error[0] == '\0') {
        snprintf(io->error, sizeof(io->error), "%s is missing", full);
    }
}

static void run_modules(state_io *io)
{
    io->section = "cpu";
    cpu_state(io);
    io->section = "apu";
    rp2a03_state(io);
    io->section = "ppu";
    ppu_state(io);
    io->section = "mapper";
    mapper_state(io);
    io->section = "video";
    video_state(io);
    io->section = "input";
    input_state(io);
    cheats_state(io); // (saves nothing: Game Genie codes are not machine state)
}

// --- the game ----------------------------------------------------------------------------------------------------

bool savestate_pending = false;
static bool save_pending = false;
static bool load_pending = false;
static void recompute_pending(void);

static char rom_base[1024]; // the ROM's path without its extension: slot files are rom_base.ssN
static uint32_t rom_crc;

uint32_t savestate_rom_crc(void)
{
    return rom_crc;
}
static int current_slot = 1;
static uint32_t load_count = 0;

static void slot_path(int slot, char *buf, size_t size)
{
    snprintf(buf, size, "%s.ss%d", rom_base, slot);
}

static bool file_time(const char *path, time_t *when)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    *when = st.st_mtime;
    return true;
}

static void set_rom(const char *rom_path, const uint8_t *data, size_t size);

void savestate_set_rom(const char *rom_path)
{
    set_rom(rom_path, NULL, 0);
}

void savestate_set_rom_data(const char *rom_path, const uint8_t *data, size_t size)
{
    set_rom(rom_path, data, size);
}

static void set_rom(const char *rom_path, const uint8_t *data, size_t size)
{
    save_pending = load_pending = false; // (asked for in the game that was running before)
    recompute_pending();
    snprintf(rom_base, sizeof(rom_base), "%s", rom_path);
    char *slash = strrchr(rom_base, '/');
    char *dot = strrchr(rom_base, '.');
    if (dot != NULL && (slash == NULL || dot > slash)) {
        *dot = '\0';
    }
    rom_crc = 0;
    uint8_t *unzipped = NULL;
    if (data == NULL && zip_is_zip_name(rom_path) &&
        zip_extract_nes(rom_path, &unzipped, &size, NULL, 0, NULL, 0)) { // the game's CRC, not the ZIP's
        data = unzipped;
    }
    FILE *f = data != NULL ? NULL : fopen(rom_path, "rb");
    if (data != NULL) {
        rom_crc = crc32_update(0xFFFFFFFFu, data, size) ^ 0xFFFFFFFFu;
    }
    if (f != NULL) {
        uint8_t buf[16384];
        uint32_t crc = 0xFFFFFFFFu;
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            crc = crc32_update(crc, buf, n);
        }
        fclose(f);
        rom_crc = crc ^ 0xFFFFFFFFu;
    }
    free(unzipped);
    // start at the slot saved most recently
    current_slot = 1;
    time_t newest = 0;
    for (int slot = 1; slot <= SLOTS; slot++) {
        char path[1100];
        time_t when;
        slot_path(slot, path, sizeof(path));
        if (file_time(path, &when) && when > newest) {
            newest = when;
            current_slot = slot;
        }
    }
}

// Messages: the overlay on the game window, and an info message (stderr, or the debugger's log file)
static void tell_user(const char *overlay, const char *detail)
{
    video_overlay("%s", overlay);
    if (detail != NULL && detail[0] != '\0') {
        LOG_INFO(SAVESTATE, "%s (%s)", overlay, detail);
    } else {
        LOG_INFO(SAVESTATE, "%s", overlay);
    }
}

// The same for a failure: a warning, so the debugger's command window shows it too
static void tell_failure(const char *overlay, const char *detail)
{
    video_overlay("%s", overlay);
    LOG_WARN(SAVESTATE, "%s (%s)", overlay, detail);
}

// --- saving ------------------------------------------------------------------------------------------------------

static int save_slot_number; // 0: save_file
static char save_file[1100];
static uint32_t save_frame;

// The state as a file image (malloc'd into *data); false: *error says why
static bool build_state(uint8_t **data, size_t *size, char *error, size_t error_size)
{
    state_io io;
    memset(&io, 0, sizeof(io));
    io.mode = MODE_SAVE;
    out_bytes(&io, MAGIC, 8);
    out_u32(&io, FORMAT_VERSION);
    out_u32(&io, rom_crc);
    size_t count_at = io.out_size;
    out_u32(&io, 0); // the number of variables, filled in below
    run_modules(&io);
    if (io.error[0] != '\0') {
        snprintf(error, error_size, "%s", io.error);
        free(io.out);
        return false;
    }
    uint8_t *c = io.out + count_at;
    c[0] = (uint8_t)io.count;
    c[1] = (uint8_t)(io.count >> 8);
    c[2] = (uint8_t)(io.count >> 16);
    c[3] = (uint8_t)(io.count >> 24);
    *data = io.out;
    *size = io.out_size;
    return true;
}

static bool write_state(const char *path, char *error, size_t error_size)
{
    uint8_t *data;
    size_t size;
    if (!build_state(&data, &size, error, error_size)) {
        return false;
    }
    // write a new file and rename it over the old one: a slot is never left half written
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f != NULL && fwrite(data, 1, size, f) == size;
    if (f != NULL && fclose(f) != 0) {
        ok = false;
    }
    free(data);
    if (ok) {
        remove(path); // (rename does not replace an existing file on Windows)
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        snprintf(error, error_size, "cannot write %s", path);
    }
    return ok;
}

static void do_save(void)
{
    save_pending = false;
    recompute_pending();
    char path[1100];
    char error[1400];
    if (save_slot_number > 0) {
        slot_path(save_slot_number, path, sizeof(path));
    } else {
        snprintf(path, sizeof(path), "%s", save_file);
    }
    char message[64];
    if (write_state(path, error, sizeof(error))) {
        script_event(SCRIPT_EVENT_STATE_SAVED);
        if (save_slot_number > 0) {
            snprintf(message, sizeof(message), "Saved to slot %d", save_slot_number);
            tell_user(message, path);
        } else {
            LOG_INFO(SAVESTATE, "State saved to %s", path);
        }
    } else {
        if (save_slot_number > 0) {
            snprintf(message, sizeof(message), "Slot %d: cannot save", save_slot_number);
            tell_failure(message, error);
        } else {
            LOG_ERROR(SAVESTATE, "%s", error);
        }
    }
}

static bool in_emulation(void)
{
    cothread_t active = co_active();
    return active == cpu_get_thread() || active == ppu_thread;
}

void savestate_save_slot(void)
{
    if (!in_emulation()) {
        tell_user("Cannot save while the debugger has stopped the game", NULL);
        return;
    }
    save_slot_number = current_slot;
    save_frame = 0;
    save_pending = true;
    recompute_pending();
}

// The debugger, stopped: the save is taken when the emulation next reaches a quiet point (the end of the visible
// picture, line 240): one-debugger runs to the next vblank right after asking ('ss save' = save + 'fs')
void savestate_request_save_slot(int slot)
{
    save_slot_number = slot;
    save_frame = 0;
    save_pending = true;
    recompute_pending();
}

bool savestate_save_requested(void)
{
    return save_pending;
}

void savestate_save_file_at_frame(const char *path, uint32_t frame)
{
    snprintf(save_file, sizeof(save_file), "%s", path);
    save_slot_number = 0;
    save_frame = frame;
    save_pending = true;
    recompute_pending();
}

// --- loading -----------------------------------------------------------------------------------------------------

static uint8_t *load_data = NULL; // the file being loaded (checked)
static size_t load_size;
static field *load_fields = NULL;
static uint32_t load_field_count;
static int load_slot_number;
static bool load_hook_first; // the restarted CPU calls the debugger's boundary hook before the first instruction

static void free_load(void)
{
    free(load_data);
    free(load_fields);
    load_data = NULL;
    load_fields = NULL;
}

// Read and check a state file (the ROM, the version, every variable). On success it is kept for apply_load().
static bool check_loaded(const char *path, char *error, size_t error_size);

static bool read_state(const char *path, char *error, size_t error_size, bool *empty)
{
    *empty = false;
    free_load();
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        *empty = true;
        snprintf(error, error_size, "no file %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 20) {
        fclose(f);
        snprintf(error, error_size, "%s is not a save state", path);
        return false;
    }
    load_data = malloc((size_t)size);
    load_size = (size_t)size;
    bool read_ok = load_data != NULL && fread(load_data, 1, load_size, f) == load_size;
    fclose(f);
    if (!read_ok) {
        snprintf(error, error_size, "cannot read %s", path);
        free_load();
        return false;
    }
    return check_loaded(path, error, error_size);
}

// load_data / load_size hold a state (from a file or from a script): check it and index its fields
static bool check_loaded(const char *path, char *error, size_t error_size)
{
    if (load_size < 20 || memcmp(load_data, MAGIC, 8) != 0) {
        snprintf(error, error_size, "%s is not a save state", path);
        free_load();
        return false;
    }
    if (in_u32(load_data + 8) != FORMAT_VERSION) {
        snprintf(error, error_size, "made by a different version of ONE (format %u)", in_u32(load_data + 8));
        free_load();
        return false;
    }
    if (in_u32(load_data + 12) != rom_crc) {
        snprintf(error, error_size, "made with another game (ROM CRC-32 %08X, this one is %08X)",
                 in_u32(load_data + 12), rom_crc);
        free_load();
        return false;
    }
    load_field_count = in_u32(load_data + 16);
    load_fields = calloc(load_field_count ? load_field_count : 1, sizeof(field));
    size_t pos = 20;
    bool damaged = load_fields == NULL;
    for (uint32_t i = 0; i < load_field_count && !damaged; i++) {
        size_t len = (pos + 2 <= load_size) ? ((size_t)load_data[pos] | ((size_t)load_data[pos + 1] << 8)) : 0;
        if (pos + 2 + len + 4 > load_size || len == 0 || len >= sizeof(load_fields[i].name)) {
            damaged = true;
            break;
        }
        memcpy(load_fields[i].name, load_data + pos + 2, len);
        load_fields[i].name[len] = '\0';
        load_fields[i].size = in_u32(load_data + pos + 2 + len);
        load_fields[i].offset = pos + 2 + len + 4;
        if (load_fields[i].size > load_size - load_fields[i].offset) {
            damaged = true;
            break;
        }
        pos = load_fields[i].offset + load_fields[i].size;
    }
    if (damaged || pos != load_size) {
        snprintf(error, error_size, "%s is damaged", path);
        free_load();
        return false;
    }
    // every variable of this version must be there, with the same size, and nothing else
    state_io io;
    memset(&io, 0, sizeof(io));
    io.mode = MODE_CHECK;
    io.in = load_data;
    io.fields = load_fields;
    io.field_count = load_field_count;
    run_modules(&io);
    for (uint32_t i = 0; i < load_field_count && io.error[0] == '\0'; i++) {
        if (!load_fields[i].used) {
            snprintf(io.error, sizeof(io.error), "%s is unknown", load_fields[i].name);
        }
    }
    if (io.error[0] != '\0') {
        snprintf(error, error_size, "made by a different version of ONE: %s", io.error);
        free_load();
        return false;
    }
    return true;
}

// Runs on its own coroutine: replaces the emulation's coroutines (one of which may have asked for the load),
// then continues with the new CPU coroutine, or returns to whoever asked from outside the emulation.
static cothread_t loader_thread = NULL;
static cothread_t loader_return = NULL; // NULL: continue the emulation

static void apply_load(void)
{
    state_io io;
    memset(&io, 0, sizeof(io));
    io.mode = MODE_APPLY;
    io.in = load_data;
    io.fields = load_fields;
    io.field_count = load_field_count;
    run_modules(&io);
    free_load();
    cpu_state_restart(load_hook_first);
    ppu_state_restart();
    rp2a03_init(); // a fresh APU coroutine starts at the top of its per-cycle loop, where the old one was
    load_count++;
    script_event(SCRIPT_EVENT_STATE_LOADED);
}

static void loader_entry(void)
{
    for (;;) {
        apply_load();
        co_switch(loader_return != NULL ? loader_return : cpu_get_thread());
    }
}

static void run_loader(bool from_emulation)
{
    load_hook_first = from_emulation;
    loader_return = from_emulation ? NULL : co_active();
    if (loader_thread == NULL) {
        loader_thread = co_create(256 * 1024, loader_entry);
    }
    co_switch(loader_thread); // (from the emulation: never comes back here, that coroutine is gone)
}

static void loaded_message(void)
{
    char message[64];
    snprintf(message, sizeof(message), "Loaded slot %d", load_slot_number);
    tell_user(message, NULL);
}

void savestate_load_slot(void)
{
    char path[1100];
    char error[1400];
    char message[64];
    bool empty;
    slot_path(current_slot, path, sizeof(path));
    if (!read_state(path, error, sizeof(error), &empty)) {
        if (empty) {
            snprintf(message, sizeof(message), "Slot %d is empty", current_slot);
            tell_user(message, NULL);
        } else {
            snprintf(message, sizeof(message), "Slot %d: cannot load", current_slot);
            tell_failure(message, error);
        }
        return;
    }
    load_slot_number = current_slot;
    save_pending = false; // a save asked for before this load would save the wrong moment
    if (in_emulation()) {
        load_pending = true; // at the next instruction boundary
        recompute_pending();
    } else {
        run_loader(false); // the debugger has stopped the emulation: load now, it stays stopped
        loaded_message();
    }
}

bool savestate_load_file(const char *path)
{
    char error[1400];
    bool empty;
    if (!read_state(path, error, sizeof(error), &empty)) {
        LOG_ERROR(SAVESTATE, "cannot load the state: %s", error);
        return false;
    }
    run_loader(false);
    LOG_INFO(SAVESTATE, "State loaded from %s", path);
    return true;
}

// --- states in memory (scripts) ----------------------------------------------------------------------------------

static savestate_captured_fn capture_done = NULL;
static void *capture_context = NULL;

void savestate_capture(savestate_captured_fn done, void *context)
{
    capture_done = done;
    capture_context = context;
    recompute_pending();
}

bool savestate_load_memory(const uint8_t *data, size_t size)
{
    char error[1400];
    free_load();
    load_data = malloc(size ? size : 1);
    if (load_data == NULL) {
        return false;
    }
    memcpy(load_data, data, size);
    load_size = size;
    if (!check_loaded("the script's state", error, sizeof(error))) {
        LOG_WARN(SAVESTATE, "%s", error);
        return false;
    }
    save_pending = false;
    if (in_emulation()) {
        load_pending = true; // at the next instruction boundary
        load_slot_number = 0;
        recompute_pending();
    } else {
        run_loader(false);
    }
    return true;
}

// --- at instruction boundaries -----------------------------------------------------------------------------------

static bool quiet_stop = false;  // libretro: stop at the next quiet point ...
static bool quiet_armed = false; // ... after the emulation has left the one it was at
static void (*quiet_hook)(void) = NULL;

static void recompute_pending(void)
{
    savestate_pending = save_pending || load_pending || capture_done != NULL || quiet_stop;
}

void savestate_stop_at_quiet(void (*hook)(void))
{
    quiet_hook = hook;
    quiet_stop = hook != NULL;
    quiet_armed = false;
    recompute_pending();
}

bool savestate_wants_ppu(void)
{
    return load_pending || capture_done != NULL || quiet_stop ||
           (save_pending && ppu_debug_get_frame_num() + 2 >= save_frame);
}

bool savestate_save_buffer(uint8_t **data, size_t *size)
{
    char error[256];
    if (!ppu_state_quiet()) {
        return false;
    }
    if (!build_state(data, size, error, sizeof(error))) {
        LOG_WARN(SAVESTATE, "%s", error);
        return false;
    }
    return true;
}

void savestate_boundary(void)
{
    if (load_pending) {
        load_pending = false;
        recompute_pending();
        if (load_slot_number > 0) {
            loaded_message(); // (not for a script's state)
        }
        run_loader(true); // continues in the new CPU coroutine
        return;           // (not reached)
    }
    if (quiet_stop) {
        if (!ppu_state_quiet()) {
            quiet_armed = true;
        } else if (quiet_armed) {
            quiet_stop = false;
            recompute_pending();
            quiet_hook(); // (gives control back to the host; returns when the next frame is asked for)
        }
    }
    if (save_pending && ppu_state_quiet() && ppu_debug_get_frame_num() >= save_frame) {
        do_save();
    }
    if (capture_done != NULL && ppu_state_quiet()) {
        uint8_t *data = NULL;
        size_t size = 0;
        char error[256];
        savestate_captured_fn done = capture_done;
        capture_done = NULL;
        recompute_pending();
        if (!build_state(&data, &size, error, sizeof(error))) {
            LOG_WARN(SAVESTATE, "%s", error);
        }
        done(data, size, capture_context); // (data NULL on failure; otherwise the callee owns it)
    }
}

// --- slots -------------------------------------------------------------------------------------------------------

int savestate_slot(void)
{
    return current_slot;
}

void savestate_change_slot(int delta)
{
    current_slot = ((current_slot - 1 + delta) % SLOTS + SLOTS) % SLOTS + 1;
    char path[1100];
    char message[96];
    time_t when;
    slot_path(current_slot, path, sizeof(path));
    if (file_time(path, &when)) {
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", localtime(&when));
        snprintf(message, sizeof(message), "Slot %d: %s", current_slot, stamp);
    } else {
        snprintf(message, sizeof(message), "Slot %d: empty", current_slot);
    }
    tell_user(message, NULL);
}

void savestate_set_slot(int slot)
{
    if (slot >= 1 && slot <= SLOTS) {
        current_slot = slot;
    }
}

bool savestate_slot_time(int slot, char *text, size_t text_size)
{
    char path[1100];
    time_t when;
    slot_path(slot, path, sizeof(path));
    if (!file_time(path, &when)) {
        snprintf(text, text_size, "empty");
        return false;
    }
    strftime(text, text_size, "%Y-%m-%d %H:%M", localtime(&when));
    return true;
}

uint32_t savestate_load_count(void)
{
    return load_count;
}
