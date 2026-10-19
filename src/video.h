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
#ifndef video_h_defined
#define video_h_defined

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIDEO_WIDTH  256
#define VIDEO_HEIGHT 240

// Open the output window (SDL2 builds) unless headless is set. Returns false if a window was wanted but could not
// be created; emulation can still run headless in that case. Builds without SDL are always headless.
bool video_init(bool headless);
void video_shutdown(void);
bool video_is_headless(void);
void video_set_throttle(bool enable); // pace presented frames to NES speed (default on; -u turns it off)

// The PPU writes NES palette indices (0-63) here. They are converted to RGB only when a frame is presented, and
// not at all when headless, so drawing a pixel is a single byte store.
extern uint8_t video_framebuffer[VIDEO_WIDTH * VIDEO_HEIGHT];

static inline void video_draw_pixel(uint32_t x, uint32_t y, uint8_t palette_index)
{
    video_framebuffer[y * VIDEO_WIDTH + x] = palette_index;
}

void video_update_screen(void); // present the finished frame (and wait for its time slot)
// The libretro core: HOOK runs (in the PPU's coroutine) when a frame is complete, after it has been shown or, when
// headless, kept. NULL = none.
void video_set_frame_hook(void (*hook)(void));
const uint8_t *video_get_framebuffer(void);
// Scripts' drawings (script.c): a 256x240 ARGB layer (alpha 255 = opaque, 0 = transparent) blended over the
// picture in the window, or NULL for none. Screenshots show the picture without it.
void video_set_hud(const uint32_t *argb);
// The mouse over the game window in NES pixels (outside the picture: <0 or >255/239); buttons: 1 left, 2 middle,
// 4 right. false: no window.
bool video_mouse(int *x, int *y, unsigned *buttons); // VIDEO_WIDTH x VIDEO_HEIGHT NES palette indices
uint32_t video_palette_rgb(uint8_t palette_index);   // 0x00RRGGBB for a palette index

// one-debugger, stopped: show the frame in progress (the pixels drawn so far, the rest -- still the previous frame's
// -- dimmed, the beam at SCANLINE/DOT marked), or the last complete frame again
void video_show_in_progress(int scanline, int dot);
void video_show_last_frame(void);

// Screenshots: the last completed frame as a 256x240 PNG. PATH NULL = next to the ROM, numbered: game.nes ->
// game-001.png, game-002.png ... (the first name not taken). Writes the file's name into SAVED (if not NULL).
void video_set_rom_path(const char *rom_path); // emu_power_on() tells it which ROM is running
bool video_screenshot(const char *path, char *saved, size_t saved_size);

// The window title: NAME, then " - STATUS" if STATUS is not empty, then " - 60.1 fps" while the FPS counter is
// on. 'one' leaves it at "ONE"; the debugger shows "ONE Debugger - Running" / "... - Stopped".
void video_set_title(const char *name, const char *status);

// FPS counter: frames shown per second, in the window title (updated once a second by video_fps_tick()).
void video_set_fps(bool show);
bool video_fps_shown(void);

// The lag counter (input.h) on the picture, top left, like FCEUX's: "Lag 123", red on a frame in which the game did
// not poll the controllers. Over the picture in the window only (screenshots do not have it).
void video_set_lag_counter(bool show);
bool video_lag_counter_shown(void);
void video_fps_tick(void); // called by input_poll(): every frame while running, and while the debugger waits

// A short message drawn on the game window's picture for 2.5 s (not in screenshots). video_overlay_tick() (called
// by input_poll()) removes it, and draws it while no frames are coming (the debugger has stopped the game).
void video_overlay(const char *format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void video_overlay_tick(void);
void video_overlay_for(unsigned ms, const char *text); // a message shown for MS milliseconds

// video.* in one.cfg (settings.h); the full screen hotkey; after the menu (frontend.h) has closed
#include "settings.h"
const setting_group *video_settings(void);
// one-debugger, before settings_load(): video.overscan defaults to 0 there, and video.debugger_overscan (when set)
// is used instead of it
void video_set_debugger(bool is_debugger);
void video_toggle_fullscreen(void);
void video_menu_closed(void);


#endif
