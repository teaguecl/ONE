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
// Frame-in-progress view test (ctest frame_in_progress): the debugger's rs shows the frame being drawn, with what
// is not drawn yet (still the previous frame) dimmed and the beam marked (video_show_in_progress()). Reads the
// window's pixels back on SDL's dummy video driver.
#include <stdio.h>
#include <string.h>
#define SDL_MAIN_HANDLED // a plain main(), also on Windows
#include <SDL.h>
#include "settings.h"
#include "video.h"

static int failures = 0;

static void expect(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

static unsigned char *rgb;
static int out_w, out_h;

static void read_window(void)
{
    SDL_Renderer *renderer = SDL_GetRenderer(SDL_GetWindowFromID(1));
    SDL_GetRendererOutputSize(renderer, &out_w, &out_h);
    SDL_free(rgb);
    rgb = SDL_malloc((size_t)out_w * (size_t)out_h * 3);
    SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGB24, rgb, out_w * 3);
}

// the color of NES pixel X, Y in the window (0xRRGGBB), from the middle of its square
static unsigned pixel(int x, int y)
{
    int px = (x * out_w + out_w / 2) / VIDEO_WIDTH;
    int py = (y * out_h + out_h / 2) / VIDEO_HEIGHT;
    const unsigned char *p = rgb + ((size_t)py * (size_t)out_w + (size_t)px) * 3;
    return ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
}

static int bright(unsigned c)
{
    return ((c >> 16) & 0xFF) > 0xC0 && ((c >> 8) & 0xFF) > 0xC0;
}

int main(void)
{
    char error[100];
    settings_set_key("video.overscan", "0", error, sizeof(error)); // the whole picture, as in one-debugger
    if (!video_init(false) || video_is_headless()) {
        printf("no window (set SDL_VIDEODRIVER=dummy)\n");
        return 2;
    }
    video_set_throttle(false);
    memset(video_framebuffer, 0x30, sizeof(video_framebuffer)); // white everywhere
    video_update_screen();                                      // (the last complete frame)

    video_show_in_progress(100, 129); // stopped on line 100, 128 pixels of it drawn
    read_window();
    expect("lines above the beam: as drawn", bright(pixel(10, 50)) && bright(pixel(200, 99)));
    expect("the beam's line before the beam: as drawn", bright(pixel(64, 100)) && bright(pixel(127, 100)));
    expect("the beam's line after the beam: dimmed", !bright(pixel(200, 100)) && pixel(200, 100) != 0);
    expect("lines below the beam: dimmed", !bright(pixel(10, 150)) && !bright(pixel(250, 239)));
    expect("the beam marked", pixel(128, 100) == 0xFFFFFF);
    expect("ticks at both ends of the beam's line", pixel(1, 100) == 0xFFD700 && pixel(254, 100) == 0xFFD700);

    video_show_in_progress(261, 10); // the pre-render line: nothing of the new frame yet
    read_window();
    expect("pre-render line: all dimmed", !bright(pixel(10, 10)) && !bright(pixel(250, 230)));

    video_show_in_progress(245, 10); // vertical blank: the frame is complete
    read_window();
    expect("vertical blank: all as drawn", bright(pixel(10, 10)) && bright(pixel(250, 230)));

    video_show_in_progress(100, 129);
    video_show_last_frame();
    read_window();
    expect("back to the last complete frame", bright(pixel(200, 100)) && bright(pixel(10, 150)));

    // video.overscan (one's default): the window shows picture columns 8-247 and rows 8-231
    settings_set_key("video.overscan", "8", error, sizeof(error));
    memset(video_framebuffer, 0x0F, sizeof(video_framebuffer)); // black, but for picture column 8 and row 8
    for (int i = 0; i < VIDEO_HEIGHT; i++) {
        video_framebuffer[i * VIDEO_WIDTH + 8] = 0x30;
    }
    memset(video_framebuffer + 8 * VIDEO_WIDTH, 0x30, VIDEO_WIDTH);
    video_update_screen();
    read_window();
    const unsigned char *left = rgb + ((size_t)(out_h / 2) * (size_t)out_w + 1) * 3; // window column 1, middle row
    const unsigned char *top = rgb + ((size_t)1 * (size_t)out_w + (size_t)(out_w / 2)) * 3; // window row 1, middle
    expect("overscan 8: the window's left edge is picture column 8", left[0] > 0xC0 && left[1] > 0xC0);
    expect("overscan 8: the window's top edge is picture row 8", top[0] > 0xC0 && top[1] > 0xC0);
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(SDL_GetWindowFromID(1), &win_w, &win_h);
    expect("overscan 8: the window is 240x224 times the scale", win_w * 224 == win_h * 240);

    // video.debugger_overscan: one-debugger uses it instead of video.overscan when it is set; one ignores it
    video_set_debugger(true);
    settings_set_key("video.debugger_overscan", "", error, sizeof(error));
    SDL_GetWindowSize(SDL_GetWindowFromID(1), &win_w, &win_h);
    expect("debugger, debugger_overscan not set: video.overscan (240x224)", win_w * 224 == win_h * 240);
    settings_set_key("video.debugger_overscan", "0", error, sizeof(error));
    SDL_GetWindowSize(SDL_GetWindowFromID(1), &win_w, &win_h);
    expect("debugger, debugger_overscan 0: the whole picture (256x240)", win_w * 240 == win_h * 256);
    video_set_debugger(false);
    settings_set_key("video.overscan", "8", error, sizeof(error)); // (one: recomputed from video.overscan)
    SDL_GetWindowSize(SDL_GetWindowFromID(1), &win_w, &win_h);
    expect("one ignores debugger_overscan: video.overscan (240x224)", win_w * 224 == win_h * 240);

    SDL_free(rgb);
    video_shutdown();
    printf("frame_progress_test: %s\n", failures == 0 ? "all passed" : "FAILED");
    return failures == 0 ? 0 : 1;
}
