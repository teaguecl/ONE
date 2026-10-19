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
// A minimal libretro frontend for testing one_libretro.so without RetroArch: loads the core with dlopen / LoadLibrary, runs a
// game, and checks picture, sound, input and load / unload / load. Usage: libretro_host CORE.so ROM [PPM_FILE]
// (PPM_FILE: the last picture of the first run, for looking at), or libretro_host CORE.so ROM --train FRAMES [PPM_FILE]: only
// run the game for FRAMES frames, no checks (the training run of tools/pgo.sh --libretro; PPM_FILE: the last picture).
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#define LIB_OPEN(path)        ((void *)LoadLibraryA(path))
#define LIB_SYMBOL(lib, name) ((void *)GetProcAddress((HMODULE)(lib), name))
#define LIB_CLOSE(lib)        FreeLibrary((HMODULE)(lib))
static const char *lib_error(void)
{
    static char text[256];
    DWORD code = GetLastError();
    int n = snprintf(text, sizeof text, "LoadLibrary failed, error %lu: ", (unsigned long)code);
    if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, text + n,
                       (DWORD)(sizeof text - (size_t)n), NULL) == 0) {
        text[n] = '\0';
    }
    return text;
}
#define LIB_ERROR() lib_error()
#else
#include <dlfcn.h>
#define LIB_OPEN(path)        dlopen(path, RTLD_NOW | RTLD_LOCAL)
#define LIB_SYMBOL(lib, name) dlsym(lib, name)
#define LIB_CLOSE(lib)        dlclose(lib)
#define LIB_ERROR()           dlerror()
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../libretro/libretro.h"

static void *lib;
static unsigned frames_shown;
static uint32_t last_picture[256 * 240];
static unsigned picture_w, picture_h;
static uint64_t audio_frames;
static unsigned start_held;
static int failures;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL: " __VA_ARGS__);                                                                              \
            printf("\n");                                                                                              \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static unsigned controller_ports; // ports declared with SET_CONTROLLER_INFO
static unsigned zapper_offered;   // ... of which offer the Zapper
// The light gun the host pretends to hold on port 2 (libretro's coordinates)
static int gun_x, gun_y;
static bool gun_off, gun_fire;
static unsigned log_messages;
static bool log_text_ok = true;

static void log_printf(enum retro_log_level level, const char *fmt, ...)
{
    (void)level;
    log_messages++;
    log_text_ok = log_text_ok && fmt != NULL && strncmp(fmt, "[", 1) == 0; // "[category] message\n"
}

static bool environment(unsigned cmd, void *data)
{
    if (cmd == RETRO_ENVIRONMENT_SET_CONTROLLER_INFO) {
        const struct retro_controller_info *port = data;
        for (; port->types != NULL; port++) {
            bool controller =
                port->types[0].id == RETRO_DEVICE_JOYPAD && strcmp(port->types[0].desc, "NES Controller") == 0;
            // port 2 also offers the Zapper
            bool zapper = port->num_types == 2 && port->types[1].id == RETRO_DEVICE_LIGHTGUN &&
                          strcmp(port->types[1].desc, "Zapper") == 0;
            controller_ports += controller && (port->num_types == 1 || zapper);
            zapper_offered += zapper;
        }
        return true;
    }
    if (cmd == RETRO_ENVIRONMENT_GET_LOG_INTERFACE) {
        ((struct retro_log_callback *)data)->log = log_printf;
        return true;
    }
    if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) {
        return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
    }
    return cmd == RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS || cmd == RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME ||
           cmd == RETRO_ENVIRONMENT_SET_VARIABLES || cmd == RETRO_ENVIRONMENT_SET_CONTROLLER_INFO; // (GET_VARIABLE: no)
}

static void video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (data == NULL) {
        return;
    }
    picture_w = w;
    picture_h = h;
    for (unsigned y = 0; y < h; y++) {
        memcpy(&last_picture[y * w], (const char *)data + y * pitch, w * 4);
    }
    frames_shown++;
}

static void audio_sample(int16_t l, int16_t r)
{
    (void)l;
    (void)r;
    audio_frames++;
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
    (void)data;
    audio_frames += frames;
    return frames;
}

static void input_poll(void) {}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port == 1 && device == RETRO_DEVICE_LIGHTGUN) {
        switch (id) {
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X: return (int16_t)gun_x;
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y: return (int16_t)gun_y;
        case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN: return gun_off;
        case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER: return gun_fire;
        default: return 0;
        }
    }
    return port == 0 && device == RETRO_DEVICE_JOYPAD && id == RETRO_DEVICE_ID_JOYPAD_START && start_held;
}

static uint32_t crc(const uint32_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

// (a declaration of a function pointer: the arguments cannot be put in parentheses)
#define FN(ret, args, name) ret(*name) args = (ret(*) args)LIB_SYMBOL(lib, #name) // NOLINT(bugprone-macro-parentheses)

// The ROM file as the frontend hands it over (NULL: the game is loaded by its path)
static uint8_t *rom_data;
static size_t rom_size;

static void read_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    fseek(f, 0, SEEK_END);
    rom_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    rom_data = malloc(rom_size);
    if (rom_data != NULL && fread(rom_data, 1, rom_size, f) != rom_size) {
        free(rom_data);
        rom_data = NULL;
    }
    fclose(f);
}

// The last picture shown, as a PPM file (for looking at; the same game gives the same file in every build)
static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    fprintf(f, "P6\n256 240\n255\n");
    for (size_t i = 0; i < 256 * 240; i++) {
        fputc((last_picture[i] >> 16) & 255, f);
        fputc((last_picture[i] >> 8) & 255, f);
        fputc(last_picture[i] & 255, f);
    }
    fclose(f);
}

// Run the game for FRAMES frames; the picture's hash (0 on failure)
static uint32_t run_game(const char *rom, unsigned frames, bool hold_start)
{
    FN(bool, (const struct retro_game_info *), retro_load_game);
    FN(void, (void), retro_run);
    FN(void, (void), retro_unload_game);
    struct retro_game_info info = {rom, rom_data, rom_data != NULL ? rom_size : 0, NULL};
    frames_shown = 0;
    audio_frames = 0;
    start_held = hold_start;
    if (!retro_load_game(&info)) {
        printf("FAIL: retro_load_game(%s)\n", rom);
        failures++;
        return 0;
    }
    for (unsigned i = 0; i < frames; i++) {
        retro_run();
    }
    uint32_t h = crc(last_picture, 256 * 240);
    retro_unload_game();
    return h;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: libretro_host CORE.so ROM [PPM_FILE]   |   libretro_host CORE.so --version\n");
        return 2;
    }
    lib = LIB_OPEN(argv[1]);
    if (lib == NULL) {
        fprintf(stderr, "cannot load %s: %s\n", argv[1], LIB_ERROR());
        return 2;
    }
    FN(unsigned, (void), retro_api_version);
    FN(void, (retro_environment_t), retro_set_environment);
    FN(void, (retro_video_refresh_t), retro_set_video_refresh);
    FN(void, (retro_audio_sample_t), retro_set_audio_sample);
    FN(void, (retro_audio_sample_batch_t), retro_set_audio_sample_batch);
    FN(void, (retro_input_poll_t), retro_set_input_poll);
    FN(void, (retro_input_state_t), retro_set_input_state);
    FN(void, (void), retro_init);
    FN(void, (void), retro_deinit);
    FN(void, (struct retro_system_av_info *), retro_get_system_av_info);
    FN(void, (struct retro_system_info *), retro_get_system_info);
    FN(unsigned, (void), retro_get_region);

    if (strcmp(argv[2], "--version") == 0) { // just the core's name and version (tests/version_check.py)
        struct retro_system_info info;
        retro_get_system_info(&info);
        printf("libretro_host: core %s %s\n", info.library_name, info.library_version);
        return 0;
    }

    CHECK(retro_api_version() == RETRO_API_VERSION, "API version");
    retro_set_environment(environment);
    retro_set_video_refresh(video);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    CHECK(controller_ports == 2 && zapper_offered == 1, "controller info for %u ports, the Zapper on %u",
          controller_ports, zapper_offered);
    retro_init();

    struct retro_system_info sys;
    retro_get_system_info(&sys);
    CHECK(strcmp(sys.library_name, "ONE") == 0 && !sys.need_fullpath, "system info");
    struct retro_system_av_info av;
    retro_get_system_av_info(&av);
    CHECK(av.geometry.base_width == 256 && av.geometry.base_height == 240, "geometry");
    CHECK(av.timing.fps > 60.09 && av.timing.fps < 60.10, "fps %f", av.timing.fps);
    CHECK(av.timing.sample_rate == 48000.0, "sample rate");
    CHECK(retro_get_region() == RETRO_REGION_NTSC, "region");

    read_rom(argv[2]);
    CHECK(rom_data != NULL, "cannot read %s", argv[2]);
    CHECK(av.geometry.aspect_ratio > 1.33 && av.geometry.aspect_ratio < 1.34, "aspect %f", av.geometry.aspect_ratio);

    if (argc > 4 && strcmp(argv[3], "--train") == 0) {
        run_game(argv[2], (unsigned)atoi(argv[4]), false);
        if (argc > 5) {
            write_ppm(argv[5]);
        }
        return failures != 0;
    }

    // 1. a run: one picture per frame, the right amount of sound, a picture that is not blank
    const unsigned frames = 600;
    uint32_t first = run_game(argv[2], frames, false);
    CHECK(frames_shown == frames, "pictures shown: %u", frames_shown);
    CHECK(picture_w == 256 && picture_h == 240, "picture size %ux%u", picture_w, picture_h);
    double expected = frames * 48000.0 / av.timing.fps;
    CHECK((double)audio_frames > expected - 200 && (double)audio_frames < expected + 200,
          "sound: %llu samples, expected %.0f", (unsigned long long)audio_frames, expected);
    unsigned colors = 0;
    for (size_t i = 1; i < 256 * 240; i++) {
        colors += last_picture[i] != last_picture[0];
    }
    CHECK(colors > 100, "the picture is blank");
    if (argc > 3) {
        write_ppm(argv[3]);
    }

    // a file that is not a game: refused, with the reason in the frontend's log
    {
        static const uint8_t junk[64] = {1, 2, 3};
        struct retro_game_info bad = {"junk.nes", junk, sizeof(junk), NULL};
        unsigned before = log_messages;
        CHECK(!((bool (*)(const struct retro_game_info *))LIB_SYMBOL(lib, "retro_load_game"))(&bad),
              "junk was accepted");
        CHECK(log_messages > before, "no log message reached the frontend");
        CHECK(log_text_ok, "a log message is not in the form \"[category] ...\"");
    }

    // 2. load, unload, load again in the same process: the same game gives the same picture
    uint32_t second = run_game(argv[2], frames, false);
    CHECK(first == second, "second run differs (%08x, %08x)", first, second);

    // 2b. the same game given by its path alone gives the same picture
    void *kept = rom_data;
    rom_data = NULL;
    uint32_t by_path = run_game(argv[2], frames, false);
    rom_data = kept;
    CHECK(by_path == first, "loading by path differs (%08x, %08x)", by_path, first);

    // 3. input reaches the game (START changes what the game shows, in most games; the picture must at least differ
    // between two runs with different input, or the game ignores START, which is reported but not a failure)
    uint32_t with_start = run_game(argv[2], frames, true);
    printf("START held: picture %s\n", with_start != first ? "changed" : "unchanged (the game may ignore START)");

    // 4. save states: size, round trip (state at frame 100, the same frames again after loading it), system RAM, cheats
    FN(bool, (const struct retro_game_info *), retro_load_game);
    FN(void, (void), retro_run);
    FN(void, (void), retro_unload_game);
    FN(void, (void), retro_reset);
    FN(size_t, (void), retro_serialize_size);
    FN(bool, (void *, size_t), retro_serialize);
    FN(bool, (const void *, size_t), retro_unserialize);
    FN(void *, (unsigned), retro_get_memory_data);
    FN(size_t, (unsigned), retro_get_memory_size);
    FN(void, (unsigned, bool, const char *), retro_cheat_set);
    FN(void, (void), retro_cheat_reset);
    struct retro_game_info info = {argv[2], rom_data, rom_size, NULL};
    start_held = 0;
    if (!retro_load_game(&info)) {
        printf("FAIL: retro_load_game for the state test\n");
        return 1;
    }
    CHECK(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) == NULL && retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0,
          "nestest has no battery RAM");
    CHECK(retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM) != NULL &&
              retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 2048,
          "system RAM");
    for (int i = 0; i < 100; i++) {
        retro_run();
    }
    size_t size = retro_serialize_size();
    CHECK(size > 1000, "state size %zu", size);
    uint8_t *state = malloc(size);
    CHECK(retro_serialize(state, size), "retro_serialize");
    audio_frames = 0;
    for (int i = 0; i < 100; i++) {
        retro_run();
    }
    uint32_t after_a = crc(last_picture, 256 * 240);
    uint64_t audio_a = audio_frames;
    CHECK(retro_serialize_size() == size, "the state size changed");
    CHECK(retro_unserialize(state, size), "retro_unserialize");
    audio_frames = 0;
    for (int i = 0; i < 100; i++) {
        retro_run();
    }
    CHECK(crc(last_picture, 256 * 240) == after_a, "the picture after loading the state differs");
    CHECK(audio_frames == audio_a, "the sound after loading the state differs (%llu, %llu)",
          (unsigned long long)audio_frames, (unsigned long long)audio_a);
    // a state taken again at the same moment is the same bytes
    CHECK(retro_unserialize(state, size), "retro_unserialize (again)");
    uint8_t *again = malloc(size);
    for (int i = 0; i < 1; i++) {
        retro_run();
    }
    retro_unserialize(state, size);
    retro_run();
    CHECK(retro_serialize(again, size), "retro_serialize (second)");
    retro_unserialize(state, size);
    retro_run();
    uint8_t *third = malloc(size);
    CHECK(retro_serialize(third, size) && memcmp(again, third, size) == 0, "states of the same frame differ");
    free(again);
    free(third);
    // reset and cheats must not break the run (a code that changes a ROM byte, then none)
    retro_reset();
    retro_cheat_set(0, true, "SXIOPO");
    for (int i = 0; i < 30; i++) {
        retro_run();
    }
    retro_cheat_reset();
    for (int i = 0; i < 30; i++) {
        retro_run();
    }
    CHECK(retro_serialize(state, size), "retro_serialize after reset and cheats");
    free(state);
    retro_unload_game();

    // 5. battery RAM is the frontend's: a made-up MMC1 game with a battery writes $A5 to $6000 and copies $6001 to
    // $6002; the frontend (here: this test) sees the RAM through RETRO_MEMORY_SAVE_RAM and can put a saved one in
    {
        size_t image_size = 16 + 32768 + 8192;
        uint8_t *image = calloc(1, image_size);
        static const uint8_t header[16] = {'N', 'E', 'S', 0x1A, 2, 1, 0x12, 0};
        static const uint8_t code[] = {0xA9, 0xA5, 0x8D, 0x00, 0x60, 0xAD, 0x01,
                                       0x60, 0x8D, 0x02, 0x60, 0x4C, 0x05, 0xC0};
        memcpy(image, header, 16);
        memcpy(image + 16 + 16384, code, sizeof(code));
        image[16 + 32768 - 4] = 0x00; // the reset vector ($FFFC): $C000
        image[16 + 32768 - 3] = 0xC0;
        struct retro_game_info battery_game = {"battery.nes", image, image_size, NULL};
        if (!retro_load_game(&battery_game)) {
            printf("FAIL: retro_load_game for the battery game\n");
            failures++;
        } else {
            uint8_t *ram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
            CHECK(ram != NULL && retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 8192, "battery RAM %p, %zu bytes",
                  (void *)ram, retro_get_memory_size(RETRO_MEMORY_SAVE_RAM));
            if (ram != NULL) {
                ram[1] = 0x5A; // what the frontend loaded from its .srm file
                for (int i = 0; i < 10; i++) {
                    retro_run();
                }
                CHECK(ram[0] == 0xA5 && ram[2] == 0x5A, "the game did not use the battery RAM (%02x %02x)", ram[0],
                      ram[2]);
                CHECK(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) == ram, "the battery RAM moved");
            }
            retro_unload_game();
        }
        free(image);
    }

    // 6. the Zapper (--zapper ROM: tests/zapper/zapper_free.nes, which records bits 3 and 4 of every $4017 read per frame at
    // $0200 / $0380): a light gun on port 2 aimed at the white square sees light, aimed at black does not, the trigger
    // is bit 4, and a shot away from the screen sees no light
    for (int i = 3; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--zapper") != 0) {
            continue;
        }
        FN(void, (unsigned, unsigned), retro_set_controller_port_device);
        read_rom(argv[i + 1]);
        CHECK(rom_data != NULL, "cannot read %s", argv[i + 1]);
        retro_set_controller_port_device(1, RETRO_DEVICE_LIGHTGUN);
        struct {
            const char *what;
            int x, y; // NES pixels of the aim
            bool off, fire;
            bool light, trigger; // expected
        } shots[] = {
            {"the white square", 136, 104, false, false, true, false},
            {"black", 20, 20, false, false, false, false},
            {"the white square, trigger", 136, 104, false, true, true, true},
            {"away from the screen, trigger", 136, 104, true, true, false, true},
        };
        for (size_t k = 0; k < sizeof(shots) / sizeof(shots[0]); k++) {
            gun_x = (int)((2 * shots[k].x + 1) * 65536.0 / 512 - 32768);
            gun_y = (int)((2 * shots[k].y + 1) * 65536.0 / 480 - 32768);
            gun_off = shots[k].off;
            gun_fire = shots[k].fire;
            struct retro_game_info zinfo = {argv[i + 1], rom_data, rom_size, NULL};
            if (!retro_load_game(&zinfo)) {
                printf("FAIL: retro_load_game for the Zapper test\n");
                failures++;
                break;
            }
            for (int f = 0; f < 90; f++) {
                retro_run();
            }
            const uint8_t *ram = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
            // the latest finished frame: 3 bytes a change (counter low, high, $4017 AND $18); 9 bytes = dark, light, dark
            bool saw_light = ram[0x380] >= 9;
            bool saw_trigger = (ram[0x202] & 0x10) != 0;
            CHECK(saw_light == shots[k].light && saw_trigger == shots[k].trigger,
                  "Zapper aimed at %s: light %d (want %d), trigger %d (want %d); %u bytes, first read %02x",
                  shots[k].what, saw_light, shots[k].light, saw_trigger, shots[k].trigger, ram[0x380], ram[0x202]);
            retro_unload_game();
        }
        retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
    }

    retro_deinit();
    LIB_CLOSE(lib);
    if (failures == 0) {
        printf("libretro_host: all passed\n");
    }
    return failures != 0;
}
