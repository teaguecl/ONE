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
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "video.h"
#include "ppu.h"
#include "script.h"
#include "input.h"
#include "png.h"
#include "font5x7.h"
#include "savestate.h"
#include "settings.h"
#include "frontend.h"
#ifdef ONE_HAVE_SDL2
#define SDL_MAIN_HANDLED /* ONE has its own main(); no SDL2main needed */
#include <SDL.h>
#endif

// taken from Nestopia, I think
static const uint8_t rgb_palette[64 * 3] = {
    0x66, 0x66, 0x66, 0x00, 0x2a, 0x88, 0x14, 0x12, 0xa7, 0x3b, 0x00, 0xa4, 0x5c, 0x00, 0x7e, 0x6e, 0x00, 0x40,
    0x6c, 0x07, 0x00, 0x56, 0x1d, 0x00, 0x33, 0x35, 0x00, 0x0c, 0x48, 0x00, 0x00, 0x52, 0x00, 0x00, 0x4f, 0x08,
    0x00, 0x40, 0x4d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xad, 0xad, 0xad, 0x15, 0x5f, 0xd9,
    0x42, 0x40, 0xff, 0x75, 0x27, 0xfe, 0xa0, 0x1a, 0xcc, 0xb7, 0x1e, 0x7b, 0xb5, 0x31, 0x20, 0x99, 0x4e, 0x00,
    0x6b, 0x6d, 0x00, 0x38, 0x87, 0x00, 0x0d, 0x93, 0x00, 0x00, 0x8f, 0x32, 0x00, 0x7c, 0x8d, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x64, 0xb0, 0xff, 0x92, 0x90, 0xff, 0xc6, 0x76, 0xff,
    0xf2, 0x6a, 0xff, 0xff, 0x6e, 0xcc, 0xff, 0x81, 0x70, 0xea, 0x9e, 0x22, 0xbc, 0xbe, 0x00, 0x88, 0xd8, 0x00,
    0x5c, 0xe4, 0x30, 0x45, 0xe0, 0x82, 0x48, 0xcd, 0xde, 0x4f, 0x4f, 0x4f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xc0, 0xdf, 0xff, 0xd3, 0xd2, 0xff, 0xe8, 0xc8, 0xff, 0xfa, 0xc2, 0xff, 0xff, 0xc4, 0xea,
    0xff, 0xcc, 0xc5, 0xf7, 0xd8, 0xa5, 0xe4, 0xe5, 0x94, 0xcf, 0xef, 0x96, 0xbd, 0xf4, 0xab, 0xb3, 0xf3, 0xcc,
    0xb5, 0xeb, 0xf2, 0xb8, 0xb8, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};


#define VIDEO_SCALE 3           // default window size: 3x the NES picture (the window can be resized)
static int scale = VIDEO_SCALE; // video.scale
static bool fullscreen = false; // video.fullscreen
// Overscan: pixels hidden at each edge of the picture in the window, as a TV's bezel hid them (screenshots, traces
// and the debugger's views keep the whole 256x240). video.overscan's default is 8 in 'one' and 0 in one-debugger
// (video_set_debugger() before settings_load()); in one-debugger video.debugger_overscan, when set, wins.
static int overscan = 8;           // in effect
static int overscan_setting = 8;   // video.overscan
static int debugger_overscan = -1; // video.debugger_overscan (-1: not set)
static bool debugger = false;      // video_set_debugger()
static char overscan_default[4] = "8";

// NTSC frame rate: master clock 236.25 MHz / 11 = 21.477 MHz, 4 per dot, 341 x 262 dots minus 0.5 on average
// (the odd-frame skipped dot) -> 39375000 / 655171 = 60.0988 frames/s.
#define NES_FRAME_RATE_NUM 39375000ULL
#define NES_FRAME_RATE_DEN 655171ULL

uint8_t video_framebuffer[VIDEO_WIDTH * VIDEO_HEIGHT]; // NES palette indices
static uint32_t palette_rgb[64];
static bool headless = true;
static bool throttle = true;
static uint8_t last_frame[VIDEO_WIDTH * VIDEO_HEIGHT]; // the last completed frame (screenshots)
static bool have_last_frame = false;
static const uint32_t *hud = NULL; // drawings over the picture (script.c), ARGB with alpha 255 = opaque
static char rom_path[1024];
static bool fps_shown = false;
static bool lag_shown = false;      // the lag counter on the picture (video_set_lag_counter())
static uint32_t fps_frames = 0;     // frames shown since fps_since
static char overlay_text[64];       // video_overlay(): the message on the picture ('' = none)
static char title_name[64] = "ONE"; // the window title: NAME [- STATUS] [- FPS] (video_set_title())
static char title_status[64] = "";
#ifdef ONE_HAVE_SDL2
static char title_fps[32] = ""; // (the FPS counter needs a window)
#endif

#ifdef ONE_HAVE_SDL2
static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;
static uint64_t next_frame_time; // SDL performance counter value at which the next frame is due
static uint64_t frame_ticks_num; // ticks per frame = perf_freq * DEN / NUM, kept as a fraction ...
static uint64_t frame_ticks_rem; // ... so the pacing does not drift (remainder accumulator)
static uint64_t fps_since;       // SDL performance counter at the last FPS update
static uint32_t overlay_until;   // SDL_GetTicks() when the message disappears
static bool overlay_dirty;       // the message appeared or disappeared since the window was last drawn
static bool frame_dirty;         // the texture does not hold last_frame (a state was loaded)
static uint32_t last_present;    // SDL_GetTicks() of the last frame drawn

// The part of the picture the window shows (all of it but the overscan)
static SDL_Rect picture_rect(void)
{
    SDL_Rect r = {overscan, overscan, VIDEO_WIDTH - 2 * overscan, VIDEO_HEIGHT - 2 * overscan};
    return r;
}

// The renderer's logical size is the shown part of the picture (keeps its aspect ratio when the window is resized)
static void set_logical_size(void)
{
    SDL_Rect r = picture_rect();
    SDL_RenderSetLogicalSize(renderer, r.w, r.h);
}

// The picture (in the texture) into the window, without the overscan
static void copy_picture(void)
{
    SDL_Rect src = picture_rect();
    SDL_RenderCopy(renderer, texture, &src, NULL);
}
#endif

static void update_title(void)
{
#ifdef ONE_HAVE_SDL2
    if (window != NULL) {
        char title[200];
        snprintf(title, sizeof(title), "%s%s%s%s%s", title_name, title_status[0] ? " - " : "", title_status,
                 title_fps[0] ? " - " : "", title_fps);
        SDL_SetWindowTitle(window, title);
    }
#endif
}

void video_set_title(const char *name, const char *status)
{
    snprintf(title_name, sizeof(title_name), "%s", name != NULL ? name : "ONE");
    snprintf(title_status, sizeof(title_status), "%s", status != NULL ? status : "");
    update_title();
}

bool video_init(bool want_headless)
{
    for (int i = 0; i < 64; i++) {
        palette_rgb[i] = ((uint32_t)rgb_palette[i * 3] << 16) | ((uint32_t)rgb_palette[i * 3 + 1] << 8) |
                         (uint32_t)rgb_palette[i * 3 + 2];
    }
    headless = true;
    if (want_headless) {
        return true;
    }
#ifdef ONE_HAVE_SDL2
    SDL_SetMainReady();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        LOG_WARN(VIDEO, "SDL_Init failed (%s), running headless", SDL_GetError());
        return false;
    }
    window = SDL_CreateWindow("ONE", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, picture_rect().w * scale,
                              picture_rect().h * scale,
                              SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (window != NULL) {
        update_title();
        // No PRESENTVSYNC: the monitor's refresh rate (60, 75, 144 Hz...) must not set the emulation speed;
        // video_update_screen() paces frames to the NES rate itself.
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (renderer == NULL) {
            renderer = SDL_CreateRenderer(window, -1, 0); // e.g. no GPU: fall back to the software renderer
        }
    }
    if (renderer != NULL) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest"); // sharp pixels when scaled
        set_logical_size();
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, VIDEO_WIDTH,
                                    VIDEO_HEIGHT);
    }
    if (texture == NULL) {
        LOG_WARN(VIDEO, "cannot create the window (%s), running headless", SDL_GetError());
        video_shutdown();
        return false;
    }
    uint64_t freq = SDL_GetPerformanceFrequency();
    frame_ticks_num = (freq * NES_FRAME_RATE_DEN) / NES_FRAME_RATE_NUM;
    frame_ticks_rem = (freq * NES_FRAME_RATE_DEN) % NES_FRAME_RATE_NUM;
    next_frame_time = SDL_GetPerformanceCounter();
    headless = false;
    return true;
#else
    LOG_INFO(VIDEO, "this build has no SDL2 support, running headless");
    return false;
#endif
}

void video_shutdown(void)
{
#ifdef ONE_HAVE_SDL2
    if (texture) {
        SDL_DestroyTexture(texture);
        texture = NULL;
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
        renderer = NULL;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = NULL;
    }
    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    }
#endif
    headless = true;
}

bool video_is_headless(void)
{
    return headless;
}

void video_set_throttle(bool enable)
{
    throttle = enable;
}

#ifdef ONE_HAVE_SDL2
// Sleep until this frame's slot. SDL_Delay() is only millisecond-accurate, so sleep to ~2 ms before the deadline
// and spin the rest. If we fall more than a few frames behind (slow machine, window dragged...), resynchronise
// rather than running fast to catch up.
static void video_wait_for_frame_slot(void)
{
    static uint64_t rem_acc = 0;
    uint64_t freq = SDL_GetPerformanceFrequency();

    next_frame_time += frame_ticks_num;
    rem_acc += frame_ticks_rem;
    if (rem_acc >= NES_FRAME_RATE_NUM) {
        rem_acc -= NES_FRAME_RATE_NUM;
        next_frame_time++;
    }

    uint64_t now = SDL_GetPerformanceCounter();
    if (now > next_frame_time + 4 * frame_ticks_num) {
        next_frame_time = now;
        return;
    }
    while (now < next_frame_time) {
        uint64_t left_ms = ((next_frame_time - now) * 1000) / freq;
        if (left_ms > 2) {
            SDL_Delay((Uint32)(left_ms - 2));
        }
        now = SDL_GetPerformanceCounter();
    }
}
#endif

#ifdef ONE_HAVE_SDL2
// Convert palette indices to RGB straight into the texture's memory (no intermediate RGB copy).
// a over b, per channel (a's alpha 0-255)
static uint32_t blend(uint32_t b, uint32_t a)
{
    uint32_t alpha = a >> 24;
    if (alpha == 255) {
        return 0xFF000000u | a;
    }
    uint32_t rb = ((((a & 0xFF00FFu) * alpha) + ((b & 0xFF00FFu) * (255 - alpha))) >> 8) & 0xFF00FFu;
    uint32_t g = ((((a & 0x00FF00u) * alpha) + ((b & 0x00FF00u) * (255 - alpha))) >> 8) & 0x00FF00u;
    return 0xFF000000u | rb | g;
}

static void upload(const uint8_t *frame)
{
    void *pixels;
    int pitch;
    if (SDL_LockTexture(texture, NULL, &pixels, &pitch) == 0) {
        const uint8_t *src = frame;
        const uint32_t *hud_row = hud;
        for (int y = 0; y < VIDEO_HEIGHT; y++) {
            uint32_t *dst = (uint32_t *)((uint8_t *)pixels + (size_t)y * (size_t)pitch);
            for (int x = 0; x < VIDEO_WIDTH; x++) {
                dst[x] = 0xFF000000u | palette_rgb[src[x] & 0x3F];
            }
            if (hud_row != NULL) { // scripts' drawings over the picture
                for (int x = 0; x < VIDEO_WIDTH; x++) {
                    if (hud_row[x] >> 24) {
                        dst[x] = blend(dst[x], hud_row[x]);
                    }
                }
                hud_row += VIDEO_WIDTH;
            }
            src += VIDEO_WIDTH;
        }
        SDL_UnlockTexture(texture);
    }
}

// 5x7 text (white unless RED, GREEN, BLUE say otherwise) on a dark box, in NES pixels (the renderer's logical size is the NES picture), with its top left
// corner at X0,Y0 (X0 < 0: centred). Over the picture in the window only: screenshots do not have it.
static void draw_text_box(int x0, int y0, const char *text, uint8_t red, uint8_t green, uint8_t blue)
{
    int len = (int)strlen(text);
    if (len > 64) {
        len = 64;
    }
    int w = len * 6 - 1 + 6;
    int h = 7 + 6;
    if (x0 < 0) {
        x0 = (picture_rect().w - w) / 2;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 176);
    SDL_Rect box = {x0, y0, w, h};
    SDL_RenderFillRect(renderer, &box);
    SDL_Rect dots[64 * 35];
    int count = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 32 || c > 126) {
            c = '?';
        }
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (font5x7[c - 32][row] & (0x10 >> col)) {
                    SDL_Rect r = {x0 + 3 + i * 6 + col, y0 + 3 + row, 1, 1};
                    dots[count++] = r;
                }
            }
        }
    }
    SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
    SDL_RenderFillRects(renderer, dots, count);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

// The message: at the bottom of the picture
static void draw_overlay(void)
{
    if (overlay_text[0] != '\0') {
        draw_text_box(-1, picture_rect().h - (7 + 6) - 8, overlay_text, 255, 255, 255);
    }
    if (lag_shown) { // the lag counter, top left; red on a frame in which the game did not poll the controllers
        char text[24];
        snprintf(text, sizeof(text), "Lag %u", (unsigned)input_lag_count());
        if (input_last_frame_lagged()) {
            draw_text_box(8, 8, text, 255, 80, 56);
        } else {
            draw_text_box(8, 8, text, 255, 255, 255);
        }
    }
}
#endif


static void (*frame_hook)(void) = NULL;

void video_set_frame_hook(void (*hook)(void))
{
    frame_hook = hook;
}

void video_update_screen(void)
{
    input_frame_done(); // the lag counter
    if (script_active) {
        script_frame_done(); // the scripts' drawings for this frame (and their timers)
    }
    memcpy(last_frame, video_framebuffer, sizeof(last_frame));
    have_last_frame = true;
    fps_frames++;
#ifdef ONE_HAVE_SDL2
    if (!headless) {
        upload(video_framebuffer);
        frame_dirty = false;
        if (overlay_text[0] != '\0' && SDL_TICKS_PASSED(SDL_GetTicks(), overlay_until)) {
            overlay_text[0] = '\0';
        }
        overlay_dirty = false;
        SDL_RenderClear(renderer);
        copy_picture();
        draw_overlay();
        if (throttle) {
            video_wait_for_frame_slot();
        }
        SDL_RenderPresent(renderer);
        last_present = SDL_GetTicks();
    }
#endif
    if (frame_hook != NULL) {
        frame_hook();
    }
}

// --- the frame in progress (one-debugger) --------------------------------------------------------------------------
// When the debugger stops in the middle of a frame, the framebuffer holds the new frame up to the beam and the
// previous frame after it. Shown as it is, with the pixels not drawn yet in this frame dimmed and the beam marked.

void video_show_in_progress(int scanline, int dot)
{
#ifdef ONE_HAVE_SDL2
    if (headless) {
        return;
    }
    // drawn so far: the lines above the beam, and on its line the pixels before it (pixel x comes out at dot x+1);
    // past the picture (lines 240-260) all of it; on the pre-render line (261) none of it
    int drawn_lines = scanline >= VIDEO_HEIGHT ? (scanline == 261 ? 0 : VIDEO_HEIGHT) : scanline;
    int drawn_on_line = (scanline < VIDEO_HEIGHT && dot > 1) ? dot - 1 : 0;
    void *pixels;
    int pitch;
    if (SDL_LockTexture(texture, NULL, &pixels, &pitch) != 0) {
        return;
    }
    for (int y = 0; y < VIDEO_HEIGHT; y++) {
        uint32_t *dst = (uint32_t *)((uint8_t *)pixels + (size_t)y * (size_t)pitch);
        const uint8_t *src = video_framebuffer + (size_t)y * VIDEO_WIDTH;
        int drawn = y < drawn_lines ? VIDEO_WIDTH : (y == scanline ? drawn_on_line : 0);
        for (int x = 0; x < VIDEO_WIDTH; x++) {
            uint32_t rgb = palette_rgb[src[x] & 0x3F];
            if (x >= drawn) { // the previous frame's pixel: under a third as bright, a little blue
                rgb = ((rgb >> 2) & 0x3F3F3Fu) + ((rgb >> 4) & 0x0F0F0Fu) + 0x000010u;
            }
            dst[x] = 0xFF000000u | rgb;
        }
        if (y == scanline) {              // the beam: short ticks at both edges of its line, and the beam itself
            for (int x = 0; x < 4; x++) { // (inside the shown part)
                dst[overscan + x] = dst[VIDEO_WIDTH - 1 - overscan - x] = 0xFFFFD700u;
            }
            if (drawn_on_line < VIDEO_WIDTH) {
                dst[drawn_on_line] = 0xFFFFFFFFu;
            }
        }
    }
    SDL_UnlockTexture(texture);
    frame_dirty = false;
    SDL_RenderClear(renderer);
    copy_picture();
    draw_overlay();
    SDL_RenderPresent(renderer);
#else
    (void)scanline;
    (void)dot;
#endif
}

void video_show_last_frame(void)
{
#ifdef ONE_HAVE_SDL2
    if (headless) {
        return;
    }
    upload(have_last_frame ? last_frame : video_framebuffer);
    frame_dirty = false;
    SDL_RenderClear(renderer);
    copy_picture();
    draw_overlay();
    SDL_RenderPresent(renderer);
#endif
}

void video_set_hud(const uint32_t *argb)
{
    hud = argb;
}

bool video_mouse(int *x, int *y, unsigned *buttons)
{
    *x = *y = -1;
    *buttons = 0;
#ifdef ONE_HAVE_SDL2
    if (!headless && window != NULL) {
        int wx, wy;
        Uint32 state = SDL_GetMouseState(&wx, &wy);
        float lx, ly;
        SDL_RenderWindowToLogical(renderer, wx, wy, &lx, &ly); // window pixels -> NES pixels (letterbox aware)
        *x = (int)lx + overscan; // (the window shows the picture from (overscan, overscan) on)
        *y = (int)ly + overscan;
        *buttons = (unsigned)state; // SDL_BUTTON_LMASK 1, MMASK 2, RMASK 4
        return true;
    }
#endif
    return false;
}

const uint8_t *video_get_framebuffer(void)
{
    return video_framebuffer;
}

uint32_t video_palette_rgb(uint8_t palette_index)
{
    return palette_rgb[palette_index & 0x3F];
}

void video_set_rom_path(const char *path)
{
    snprintf(rom_path, sizeof(rom_path), "%s", path ? path : "");
    have_last_frame = false;
}

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fclose(f);
    return true;
}

bool video_screenshot(const char *path, char *saved, size_t saved_size)
{
    char name[1100];
    if (path == NULL || *path == '\0') {
        // the ROM's name without its extension, then -NNN.png
        char base[1024];
        snprintf(base, sizeof(base), "%s", rom_path[0] ? rom_path : "one");
        char *slash = strrchr(base, '/');
#ifdef _WIN32
        char *backslash = strrchr(base, '\\');
        if (backslash != NULL && (slash == NULL || backslash > slash)) {
            slash = backslash;
        }
#endif
        char *dot = strrchr(base, '.');
        if (dot != NULL && (slash == NULL || dot > slash)) {
            *dot = '\0';
        }
        int number = 1;
        do {
            snprintf(name, sizeof(name), "%s-%03d.png", base, number++);
        } while (file_exists(name) && number < 100000);
        path = name;
    }
    static uint8_t rgb[VIDEO_WIDTH * VIDEO_HEIGHT * 3];
    const uint8_t *frame = have_last_frame ? last_frame : video_framebuffer; // (nothing shown yet: as drawn so far)
    for (int i = 0; i < VIDEO_WIDTH * VIDEO_HEIGHT; i++) {
        uint32_t c = palette_rgb[frame[i] & 0x3F];
        rgb[i * 3] = (uint8_t)(c >> 16);
        rgb[i * 3 + 1] = (uint8_t)(c >> 8);
        rgb[i * 3 + 2] = (uint8_t)c;
    }
    if (saved != NULL && saved_size > 0) {
        snprintf(saved, saved_size, "%s", path);
    }
    return png_write_rgb(path, rgb, VIDEO_WIDTH, VIDEO_HEIGHT);
}

void video_set_fps(bool show)
{
    fps_shown = show;
#ifdef ONE_HAVE_SDL2
    fps_frames = 0;
    fps_since = SDL_GetPerformanceCounter();
    snprintf(title_fps, sizeof(title_fps), "%s", show ? "... fps" : "");
    update_title();
#endif
}

bool video_fps_shown(void)
{
    return fps_shown;
}

void video_set_lag_counter(bool show)
{
    lag_shown = show;
#ifdef ONE_HAVE_SDL2
    overlay_dirty = true; // (drawn again at once, also when the game is stopped)
#endif
}

bool video_lag_counter_shown(void)
{
    return lag_shown;
}

void video_fps_tick(void)
{
#ifdef ONE_HAVE_SDL2
    if (!fps_shown || window == NULL) {
        return;
    }
    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t freq = SDL_GetPerformanceFrequency();
    if (now - fps_since < freq) {
        return;
    }
    double fps = (double)fps_frames * (double)freq / (double)(now - fps_since);
    snprintf(title_fps, sizeof(title_fps), "%.1f fps", fps);
    update_title();
    fps_frames = 0;
    fps_since = now;
#endif
}

// --- messages on the picture -------------------------------------------------------------------------------------

void video_overlay(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(overlay_text, sizeof(overlay_text), format, args);
    va_end(args);
#ifdef ONE_HAVE_SDL2
    overlay_until = SDL_GetTicks() + 2500;
    overlay_dirty = true;
#endif
}

void video_overlay_for(unsigned ms, const char *text)
{
    video_overlay("%s", text);
#ifdef ONE_HAVE_SDL2
    overlay_until = SDL_GetTicks() + ms;
#else
    (void)ms;
#endif
}

void video_overlay_tick(void)
{
#ifdef ONE_HAVE_SDL2
    if (headless) {
        return;
    }
    uint32_t now = SDL_GetTicks();
    if (overlay_text[0] != '\0' && SDL_TICKS_PASSED(now, overlay_until)) {
        overlay_text[0] = '\0';
        overlay_dirty = true;
    }
    // Frames are being drawn (the game runs): they show the message. Otherwise (the debugger has stopped the game)
    // draw the last frame again. (SDL_TICKS_PASSED(A, B): A is at or past B: "now is within 100 ms of the last frame".)
    if ((overlay_dirty || frame_dirty) && SDL_TICKS_PASSED(last_present + 100, now)) {
        return;
    }
    if (overlay_dirty || frame_dirty) {
        if (frame_dirty) {
            upload(last_frame);
            frame_dirty = false;
        }
        overlay_dirty = false;
        SDL_RenderClear(renderer);
        copy_picture();
        draw_overlay();
        SDL_RenderPresent(renderer);
    }
#endif
}

// --- settings (settings.h) ---------------------------------------------------------------------------------------

// The overscan in effect (one-debugger: video.debugger_overscan if set), and the window to match
static void update_overscan(void)
{
    overscan = (debugger && debugger_overscan >= 0) ? debugger_overscan : overscan_setting;
#ifdef ONE_HAVE_SDL2
    if (renderer != NULL) {
        set_logical_size();
        if (!fullscreen) {
            SDL_SetWindowSize(window, picture_rect().w * scale, picture_rect().h * scale);
        }
        frame_dirty = true; // drawn again with the new edges
    }
#endif
}

static bool apply_video(const setting *s, const char *value, char *error, size_t error_size)
{
    if (error_size > 0) { // (never fails)
        error[0] = '\0';
    }
    if (s->id == 2 || s->id == 3) {
        if (s->id == 2) {
            overscan_setting = atoi(value);
        } else {
            debugger_overscan = ('\0' == value[0]) ? -1 : atoi(value);
        }
        update_overscan();
    } else if (s->id == 0) {
        scale = atoi(value);
#ifdef ONE_HAVE_SDL2
        if (window != NULL && !fullscreen) {
            SDL_SetWindowSize(window, picture_rect().w * scale, picture_rect().h * scale);
        }
#endif
    } else if (s->id == 4) {
        bool disable = false;
        settings_parse_bool(value, &disable);
        ppu_set_sprite_limit(disable);
    } else {
        settings_parse_bool(value, &fullscreen);
#ifdef ONE_HAVE_SDL2
        if (window != NULL) {
            SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            if (!fullscreen) {
                SDL_SetWindowSize(window, picture_rect().w * scale, picture_rect().h * scale);
            }
        }
#endif
    }
    return true;
}

static const char *const debugger_overscan_choices[] = {"",  "0",  "1",  "2",  "3",  "4",  "5",  "6",  "7", "8",
                                                        "9", "10", "11", "12", "13", "14", "15", "16", NULL};

static const setting video_table[] = {
    {"video.scale", SETTING_INT, "3", "Window size", NULL, "the window's size: this many times the NES picture (1-8)",
     1, 8, NULL, 0, apply_video, 0},
    {"video.fullscreen", SETTING_BOOL, "off", "Full screen", NULL, "on: the whole screen (the hotkey toggles it too)",
     0, 0, NULL, 0, apply_video, 1},
    {"video.overscan", SETTING_INT, overscan_default, "Overscan", NULL,
     "pixels hidden at each edge of the picture, as a TV hid them behind its bezel (0: the whole picture; "
     "one-debugger's default)",
     0, 16, NULL, 0, apply_video, 2},
    {"video.debugger_overscan", SETTING_CHOICE, "", "Overscan (debugger)", NULL,
     "one-debugger's overscan (0-16), instead of video.overscan; empty: video.overscan (one ignores it)", 0, 0,
     debugger_overscan_choices, SETTING_NO_MENU, apply_video, 3}, // (one-debugger's: not on one's menu)
    {"video.disable_sprite_limit", SETTING_BOOL, "off", "Disable Sprite Limit", NULL,
     "on: draw every sprite on a line, not just the first eight (no more flicker; the hardware's limit and the sprite\n"
     "overflow flag are unchanged for the game; a trace (-t) always uses the hardware's behavior)",
     0, 0, NULL, 0, apply_video, 4},
};

void video_set_debugger(bool is_debugger)
{
    debugger = is_debugger;
    snprintf(overscan_default, sizeof(overscan_default), "%d", is_debugger ? 0 : 8);
    update_overscan();
}

const setting_group *video_settings(void)
{
    static const setting_group group = {"picture", "Video", NULL, video_table,
                                        (int)(sizeof(video_table) / sizeof(video_table[0]))};
    return &group;
}

void video_toggle_fullscreen(void)
{
    char error[100];
    settings_set_key("video.fullscreen", fullscreen ? "off" : "on", error, sizeof(error));
}

// --- the menu (frontend.h) ---------------------------------------------------------------------------------------

void *fe_sdl_window(void)
{
#ifdef ONE_HAVE_SDL2
    return window;
#else
    return NULL;
#endif
}

void *fe_sdl_renderer(void)
{
#ifdef ONE_HAVE_SDL2
    return renderer;
#else
    return NULL;
#endif
}

// The menu draws in window pixels: the NES-sized logical coordinates are switched off while it is open (and the
// last frame is drawn into the middle of the window by hand, with the same black bars)
void fe_draw_game(void)
{
#ifdef ONE_HAVE_SDL2
    if (renderer == NULL) {
        return;
    }
    SDL_RenderSetLogicalSize(renderer, 0, 0);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (!have_last_frame) {
        return;
    }
    if (frame_dirty) {
        upload(last_frame);
        frame_dirty = false;
    }
    int w = 0;
    int h = 0;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    SDL_Rect dst;
    SDL_Rect shown = picture_rect();
    if (w * shown.h > h * shown.w) { // wider than the picture: bars left and right
        dst.h = h;
        dst.w = h * shown.w / shown.h;
    } else {
        dst.w = w;
        dst.h = w * shown.h / shown.w;
    }
    dst.x = (w - dst.w) / 2;
    dst.y = (h - dst.h) / 2;
    SDL_RenderCopy(renderer, texture, NULL, &dst);
#endif
}

void video_menu_closed(void)
{
#ifdef ONE_HAVE_SDL2
    if (renderer == NULL) {
        return;
    }
    SDL_RenderSetScale(renderer, 1.0f, 1.0f);
    set_logical_size();
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    if (have_last_frame) { // the picture again at once, without the menu
        upload(last_frame);
        SDL_RenderClear(renderer);
        copy_picture();
        draw_overlay();
        SDL_RenderPresent(renderer);
    }
    next_frame_time = SDL_GetPerformanceCounter(); // the time spent in the menu is not to be caught up
    last_present = SDL_GetTicks();
#endif
}

// --- save states -------------------------------------------------------------------------------------------------

// The picture: a state is taken just after a frame was completed, so this is that frame, shown again at once
// when the state is loaded.
void video_state(state_io *io)
{
    state_bytes(io, "framebuffer", video_framebuffer, sizeof(video_framebuffer));
    if (state_loading(io)) {
        memcpy(last_frame, video_framebuffer, sizeof(last_frame));
        have_last_frame = true;
#ifdef ONE_HAVE_SDL2
        frame_dirty = true;
#endif
    }
}
