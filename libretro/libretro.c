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
// ONE as a libretro core (RetroArch...): one_libretro.so.
//
// retro_run() resumes the emulation (the same coroutines as in 'one') and gets control back at the first quiet point
// after the picture is finished (savestate_stop_at_quiet(): an instruction boundary a few dots into line 240), the
// one place where a state can be taken: retro_serialize() / retro_unserialize() work at once, which rewind, run-ahead
// and netplay need. Phase 1-2: picture, sound, two joypads, save states, reset, Game Genie codes, system RAM.
// Phase 3: the game comes from memory (the frontend reads it, out of a ZIP too), battery RAM is the frontend's
// (RETRO_MEMORY_SAVE_RAM: it keeps the .srm file in its save directory), core options (aspect ratio, overscan, treble cut, sprite limit).
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"
#include "log.h"
#include "audio.h"
#include "cpu.h"
#include "emu.h"
#include "input.h"
#include "mapper.h"
#include "cheats.h"
#include "savestate.h"
#include "scheduler.h"
#include "settings.h"
#include "video.h"
#include "version.h"

#define SAMPLE_RATE 48000
#define NES_FPS     (AUDIO_CPU_HZ_NUM / AUDIO_CPU_HZ_DEN / 29780.5) // NTSC: 60.0988 frames per second

static retro_environment_t environment_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static bool game_loaded = false;
static bool frame_ready = false;
static size_t state_size = 0;           // retro_serialize_size(): the same for every state of a game
static uint32_t picture[VIDEO_WIDTH * VIDEO_HEIGHT];

// --- core options -----------------------------------------------------------------------------------------------

static const struct retro_variable core_options[] = {
    {"one_aspect_ratio", "Aspect ratio; 4:3|8:7 (NTSC pixels)|1:1 (square pixels)"},
    {"one_overscan", "Overscan crop; off|top and bottom 8 lines|all sides 8 pixels"},
    {"one_audio_lowpass", "14 kHz treble cut; enabled|disabled"},
    {"one_sprite_limit", "Disable Sprite Limit; off|on"},
    {NULL, NULL},
};

static double pixel_aspect = 4.0 / 3.0 * VIDEO_HEIGHT / VIDEO_WIDTH; // the shape of one NES pixel (4:3 screen)
static int crop_x = 0;                                               // the part of the picture shown
static int crop_y = 0;

static const char *option(const char *key)
{
    struct retro_variable v = {key, NULL};
    return (environment_cb != NULL && environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value != NULL) ? v.value : "";
}

static void read_options(void)
{
    const char *aspect = option("one_aspect_ratio");
    pixel_aspect = strncmp(aspect, "8:7", 3) == 0 ? 8.0 / 7.0 : strncmp(aspect, "1:1", 3) == 0 ? 1.0 : 1.25;
    const char *overscan = option("one_overscan");
    crop_x = strncmp(overscan, "all", 3) == 0 ? 8 : 0;
    crop_y = (strncmp(overscan, "top", 3) == 0 || strncmp(overscan, "all", 3) == 0) ? 8 : 0;
    settings_set_key("video.disable_sprite_limit", strcmp(option("one_sprite_limit"), "on") == 0 ? "on" : "off", NULL, 0);
    settings_set_key("audio.lowpass", strcmp(option("one_audio_lowpass"), "disabled") == 0 ? "off" : "on", NULL, 0);
}

static void get_geometry(struct retro_game_geometry *g)
{
    g->base_width = VIDEO_WIDTH - 2 * crop_x;
    g->base_height = VIDEO_HEIGHT - 2 * crop_y;
    g->max_width = VIDEO_WIDTH;
    g->max_height = VIDEO_HEIGHT;
    g->aspect_ratio = (float)((double)g->base_width / g->base_height * pixel_aspect);
}

// The libretro joypad buttons in the order of input.h's bit mask (A B Select Start Up Down Left Right)
static bool port2_gun = false; // the Zapper is plugged into port 2 (retro_set_controller_port_device)

static const unsigned pad_buttons[INPUT_BUTTONS] = {
    RETRO_DEVICE_ID_JOYPAD_A,    RETRO_DEVICE_ID_JOYPAD_B,    RETRO_DEVICE_ID_JOYPAD_SELECT,
    RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_UP,  RETRO_DEVICE_ID_JOYPAD_DOWN,
    RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
};

// --- the host side --------------------------------------------------------------------------------------------

// In the PPU's coroutine, at the end of the picture
static void frame_done(void)
{
    frame_ready = true;
}

// In the CPU's coroutine, at the quiet point after the picture: back to the host
static void quiet_reached(void)
{
    sched_break();
}

RETRO_API unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

// ONE's log messages (log.h) go to the frontend's log, with the category in front: "[mapper] loaded ..."
static retro_log_printf_t frontend_log;

static void log_to_frontend(log_category category, log_level level, const char *message, void *context)
{
    (void)context;
    enum retro_log_level retro_level = level <= LOG_LEVEL_ERROR  ? RETRO_LOG_ERROR
                                       : level == LOG_LEVEL_WARN ? RETRO_LOG_WARN
                                       : level == LOG_LEVEL_INFO ? RETRO_LOG_INFO
                                                                 : RETRO_LOG_DEBUG;
    frontend_log(retro_level, "[%s] %s\n", log_category_name(category), message);
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    environment_cb = cb;
    struct retro_log_callback log_callback;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_callback) && log_callback.log != NULL) {
        frontend_log = log_callback.log;
        log_set_hook(log_to_frontend, NULL);
        log_set_console(false); // (no frontend log: the messages stay on stderr)
    }
    bool no_game = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)core_options);
    // Both ports take the NES's standard controller (the frontend's input menu shows it by this name); port 2 also the
    // Zapper (a light gun: input.h, zapper.h)
    static const struct retro_controller_description nes_controller[] = {
        {"NES Controller", RETRO_DEVICE_JOYPAD},
    };
    static const struct retro_controller_description port2_devices[] = {
        {"NES Controller", RETRO_DEVICE_JOYPAD},
        {"Zapper", RETRO_DEVICE_LIGHTGUN},
    };
    static const struct retro_controller_info ports[] = {
        {nes_controller, 1},
        {port2_devices, 2},
        {NULL, 0},
    };
    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)ports);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)
{
    video_cb = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)
{
    audio_cb = cb;
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
    audio_batch_cb = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb)
{
    input_poll_cb = cb;
}

RETRO_API void retro_set_input_state(retro_input_state_t cb)
{
    input_state_cb = cb;
}

RETRO_API void retro_init(void)
{
}

RETRO_API void retro_deinit(void)
{
    if (frontend_log != NULL) {
        log_set_hook(NULL, NULL); // (the frontend may unload this library: nothing may call back into it)
        log_set_console(true);
        frontend_log = NULL;
    }
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "ONE";
    info->library_version = one_version();
    info->valid_extensions = "nes";
    info->need_fullpath = false; // the frontend reads the game (and unpacks a ZIP)
    info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    get_geometry(&info->geometry);
    info->timing.fps = NES_FPS;
    info->timing.sample_rate = SAMPLE_RATE;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
    if (port == 1) {
        port2_gun = (device & RETRO_DEVICE_MASK) == RETRO_DEVICE_LIGHTGUN;
        if (!port2_gun) {
            input_set_zapper(NULL);
        }
    }
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
    if (game == NULL || (game->path == NULL && game->data == NULL) || game_loaded) {
        return false;
    }
    enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environment_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
        fprintf(stderr, "ONE: the frontend does not support XRGB8888\n");
        return false;
    }
    static const struct retro_input_descriptor descriptors[] = {
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
        {1, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER, "Zapper trigger"},
        {1, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD, "Zapper trigger, off screen"},
        {0, 0, 0, 0, NULL},
    };
    environment_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)descriptors);

    settings_load_defaults(); // (no one.cfg: the core's options)
    settings_set_key("audio.volume", "100", NULL, 0); // the frontend has the volume control
    settings_set_key("audio.sample_rate", "48000", NULL, 0);
    read_options();
    mapper_set_sav_files(false); // battery RAM is the frontend's (retro_get_memory_data)
    video_init(true);
    video_set_frame_hook(frame_done);
    audio_init(true);
    audio_set_buffer(true);

    char path[4096];
    snprintf(path, sizeof(path), "%s", game->path != NULL ? game->path : "game.nes");
    bool powered = game->data != NULL && game->size > 0 ? emu_power_on_memory(game->data, game->size, path)
                                                         : emu_power_on(path);
    if (!powered) {
        audio_set_buffer(false);
        audio_shutdown();
        video_shutdown();
        return false;
    }
    state_size = 0;
    game_loaded = true;
    return true;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
    (void)type;
    (void)info;
    (void)num;
    return false;
}

RETRO_API void retro_unload_game(void)
{
    if (!game_loaded) {
        return;
    }
    emu_power_off();
    audio_set_buffer(false);
    audio_shutdown();
    video_set_frame_hook(NULL);
    video_shutdown();
    game_loaded = false;
}

RETRO_API void retro_reset(void)
{
    if (game_loaded) {
        cpu_request_soft_reset();
    }
}

// The Zapper's aim from the frontend's light gun. SCREEN_X / SCREEN_Y run -0x8000..0x7fff over the picture as shown (the
// cropped part), RELOAD is a shot away from the screen
static void poll_light_gun(void)
{
    input_zapper_state aim = {false, 0, 0, false};
    bool reload = input_state_cb(1, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD) != 0;
    bool off = reload || input_state_cb(1, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN) != 0;
    int sx = input_state_cb(1, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
    int sy = input_state_cb(1, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);
    int width = VIDEO_WIDTH - 2 * crop_x, height = VIDEO_HEIGHT - 2 * crop_y;
    int x = crop_x + (sx + 0x8000) * width / 0x10000;
    int y = crop_y + (sy + 0x8000) * height / 0x10000;
    aim.on_screen = !off && x >= 0 && x < VIDEO_WIDTH && y >= 0 && y < VIDEO_HEIGHT;
    aim.x = aim.on_screen ? x : 0;
    aim.y = aim.on_screen ? y : 0;
    aim.trigger = reload || input_state_cb(1, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER) != 0;
    input_set_zapper(&aim);
}

RETRO_API void retro_run(void)
{
    if (!game_loaded) {
        return;
    }
    input_poll_cb();
    for (int port = 0; port < INPUT_PORTS; port++) {
        if (port == 1 && port2_gun) {
            poll_light_gun();
            continue;
        }
        uint8_t buttons = 0;
        for (int b = 0; b < INPUT_BUTTONS; b++) {
            if (input_state_cb((unsigned)port, RETRO_DEVICE_JOYPAD, 0, pad_buttons[b])) {
                buttons |= (uint8_t)(1u << b);
            }
        }
        input_set_override(port, buttons);
    }

    bool updated = false;
    if (environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        read_options();
        struct retro_game_geometry geometry;
        get_geometry(&geometry);
        environment_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
    }

    frame_ready = false;
    sched_reset(); // (the host coroutine is whatever thread the frontend calls us on)
    savestate_stop_at_quiet(quiet_reached);
    sched_resume(cpu_get_thread());

    if (frame_ready) {
        uint32_t colors[64];
        for (int i = 0; i < 64; i++) {
            colors[i] = video_palette_rgb((uint8_t)i);
        }
        const uint8_t *indices = video_get_framebuffer();
        for (int i = 0; i < VIDEO_WIDTH * VIDEO_HEIGHT; i++) {
            picture[i] = colors[indices[i] & 63];
        }
        video_cb(picture + crop_y * VIDEO_WIDTH + crop_x, (unsigned)(VIDEO_WIDTH - 2 * crop_x),
                 (unsigned)(VIDEO_HEIGHT - 2 * crop_y), VIDEO_WIDTH * sizeof(uint32_t));
    } else {
        video_cb(NULL, VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_WIDTH * sizeof(uint32_t)); // (the emulation ended)
    }

    const int16_t *mono;
    size_t count = audio_take_buffer(&mono);
    int16_t stereo[2 * 4096];
    for (size_t i = 0; i < count; i++) {
        stereo[2 * i] = mono[i];
        stereo[2 * i + 1] = mono[i];
    }
    if (count > 0) {
        audio_batch_cb(stereo, count);
    }
}

// --- save states ----------------------------------------------------------------------------------------------

// The size does not change from state to state (every variable has its name and size), so one dry run tells it
RETRO_API size_t retro_serialize_size(void)
{
    if (!game_loaded) {
        return 0;
    }
    if (state_size == 0) {
        uint8_t *data;
        size_t size;
        if (savestate_save_buffer(&data, &size)) {
            state_size = size;
            free(data);
        } else {
            return 0;
        }
    }
    return state_size;
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
    uint8_t *state;
    size_t state_bytes;
    if (!game_loaded || !savestate_save_buffer(&state, &state_bytes)) {
        return false; // (before the first frame the machine is not at a quiet point)
    }
    bool ok = state_bytes == size;
    if (ok) {
        memcpy(data, state, size);
        state_size = size;
    }
    free(state);
    return ok;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
    return game_loaded && savestate_load_memory(data, size); // (at once: the host has the machine)
}

// --- cheats: Game Genie codes (the frontend's list: index, on/off, code; "CODE+CODE" for several) -----------------

#define CHEAT_TEXT_MAX 128
static struct {
    bool on;
    char code[CHEAT_TEXT_MAX];
} frontend_cheats[CHEATS_MAX];

static void apply_cheats(void)
{
    cheats_clear();
    for (int i = 0; i < CHEATS_MAX; i++) {
        if (!frontend_cheats[i].on) {
            continue;
        }
        char text[CHEAT_TEXT_MAX];
        snprintf(text, sizeof(text), "%s", frontend_cheats[i].code);
        for (char *part = strtok(text, "+"); part != NULL; part = strtok(NULL, "+")) {
            cheat_code code;
            if (cheats_decode(part, &code)) {
                cheats_add(&code, NULL);
            }
        }
    }
    cheats_set_enabled(cheats_active_count() > 0);
}

RETRO_API void retro_cheat_reset(void)
{
    memset(frontend_cheats, 0, sizeof(frontend_cheats));
    if (game_loaded) {
        apply_cheats();
    }
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    if (index >= CHEATS_MAX || code == NULL) {
        return;
    }
    frontend_cheats[index].on = enabled;
    snprintf(frontend_cheats[index].code, CHEAT_TEXT_MAX, "%s", code);
    if (game_loaded) {
        apply_cheats();
    }
}

// --- region and memory ------------------------------------------------------------------------------------------

RETRO_API unsigned retro_get_region(void)
{
    return RETRO_REGION_NTSC;
}

// SAVE_RAM: the cartridge's battery-backed RAM (the frontend loads and saves it: the .srm file); SYSTEM_RAM: the 2 KB
RETRO_API void *retro_get_memory_data(unsigned id)
{
    uint8_t *data;
    size_t size;
    if (!game_loaded) {
        return NULL;
    }
    if (id == RETRO_MEMORY_SYSTEM_RAM) {
        return cpu_internal_ram();
    }
    return id == RETRO_MEMORY_SAVE_RAM && mapper_battery_ram(&data, &size) ? data : NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
    uint8_t *data;
    size_t size;
    if (!game_loaded) {
        return 0;
    }
    if (id == RETRO_MEMORY_SYSTEM_RAM) {
        return CPU_INTERNAL_RAM_SIZE;
    }
    return id == RETRO_MEMORY_SAVE_RAM && mapper_battery_ram(&data, &size) ? size : 0;
}
