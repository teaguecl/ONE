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
#include "script_window.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ONE_HAVE_SDL2
#include <SDL.h>
#endif
#include "png.h"
#include "video.h"

#define MAX_WINDOWS 8

typedef struct {
    const void *owner; // NULL: a free slot
    int width, height;
    uint32_t *pixels;
    bool closed_by_user;                   // the window is gone (its picture stays until the script closes it)
    char keys[SCRIPT_WINDOW_MAX_KEYS][24]; // (copies: SDL_GetKeyName()'s string lasts only until its next call)
    int key_n;
    bool mouse_in; // the mouse is over the picture, at (mouse_x, mouse_y)
    int mouse_x, mouse_y;
    unsigned mouse_buttons;
#ifdef ONE_HAVE_SDL2
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
#endif
} script_window;

static script_window windows[MAX_WINDOWS];

static script_window *find(const void *owner, int id)
{
    if (id < 1 || id > MAX_WINDOWS || owner == NULL || windows[id - 1].owner != owner) {
        return NULL;
    }
    return &windows[id - 1];
}

bool script_windows_shown(void)
{
#ifdef ONE_HAVE_SDL2
    return !video_is_headless();
#else
    return false;
#endif
}

static void destroy_window(script_window *w)
{
#ifdef ONE_HAVE_SDL2
    if (w->texture != NULL) {
        SDL_DestroyTexture(w->texture);
    }
    if (w->renderer != NULL) {
        SDL_DestroyRenderer(w->renderer);
    }
    if (w->window != NULL) {
        SDL_DestroyWindow(w->window);
    }
    w->texture = NULL;
    w->renderer = NULL;
    w->window = NULL;
#else
    (void)w;
#endif
}

int script_window_open(const void *owner, const char *title, int width, int height, double scale, char *error,
                       size_t error_size)
{
    script_window *w = NULL;
    for (int i = 0; i < MAX_WINDOWS && w == NULL; i++) {
        if (windows[i].owner == NULL) {
            w = &windows[i];
        }
    }
    if (w == NULL) {
        snprintf(error, error_size, "too many windows (%d)", MAX_WINDOWS);
        return 0;
    }
    if (width < 1 || height < 1 || width > 4096 || height > 4096) {
        snprintf(error, error_size, "a window is 1 to 4096 pixels wide and high, not %dx%d", width, height);
        return 0;
    }
    if (scale < 0 || scale > 16) {
        snprintf(error, error_size, "the scale is 0 (automatic) or more, up to 16, not %g", scale);
        return 0;
    }
    memset(w, 0, sizeof(*w));
    w->pixels = calloc((size_t)width * (size_t)height, sizeof(uint32_t));
    if (w->pixels == NULL) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    w->owner = owner;
    w->width = width;
    w->height = height;
#ifdef ONE_HAVE_SDL2
    if (script_windows_shown()) {
        // SCALE times the picture (automatic: twice when that fits the screen, NES pictures are small); resizable,
        // sharp pixels
        if (scale == 0) {
            SDL_Rect usable = {0, 0, 1280, 800};
            SDL_GetDisplayUsableBounds(0, &usable);
            scale = (width * 2 <= usable.w - 40 && height * 2 <= usable.h - 80) ? 2 : 1;
        }
        int shown_w = (int)(width * scale + 0.5), shown_h = (int)(height * scale + 0.5);
        w->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, shown_w < 1 ? 1 : shown_w,
                                     shown_h < 1 ? 1 : shown_h, SDL_WINDOW_RESIZABLE);
        if (w->window != NULL) {
            w->renderer = SDL_CreateRenderer(w->window, -1, SDL_RENDERER_ACCELERATED);
            if (w->renderer == NULL) {
                w->renderer = SDL_CreateRenderer(w->window, -1, 0);
            }
        }
        if (w->renderer != NULL) {
            // sharp pixels when made larger; smaller, blended (else whole rows go: thin lines would vanish)
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scale < 1 ? "linear" : "nearest");
            SDL_RenderSetLogicalSize(w->renderer, width, height);
            w->texture =
                SDL_CreateTexture(w->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        }
        if (w->texture == NULL) {
            snprintf(error, error_size, "cannot open a window: %s", SDL_GetError());
            destroy_window(w);
            free(w->pixels);
            memset(w, 0, sizeof(*w));
            return 0;
        }
        script_window_show(owner, (int)(w - windows) + 1);
    }
#else
    (void)title;
#endif
    return (int)(w - windows) + 1;
}

bool script_window_exists(const void *owner, int id)
{
    return find(owner, id) != NULL;
}

bool script_window_closed_by_user(const void *owner, int id)
{
    script_window *w = find(owner, id);
    return w == NULL || w->closed_by_user;
}

void script_window_size(const void *owner, int id, int *width, int *height)
{
    script_window *w = find(owner, id);
    *width = w ? w->width : 0;
    *height = w ? w->height : 0;
}

uint32_t *script_window_pixels(const void *owner, int id)
{
    script_window *w = find(owner, id);
    return w ? w->pixels : NULL;
}

void script_window_show(const void *owner, int id)
{
#ifdef ONE_HAVE_SDL2
    script_window *w = find(owner, id);
    if (w == NULL || w->texture == NULL) {
        return;
    }
    SDL_UpdateTexture(w->texture, NULL, w->pixels, w->width * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(w->renderer, 0, 0, 0, 255);
    SDL_RenderClear(w->renderer);
    SDL_RenderCopy(w->renderer, w->texture, NULL, NULL);
    SDL_RenderPresent(w->renderer);
#else
    (void)owner, (void)id;
#endif
}

void script_window_close(const void *owner, int id)
{
    script_window *w = find(owner, id);
    if (w != NULL) {
        destroy_window(w);
        free(w->pixels);
        memset(w, 0, sizeof(*w));
    }
}

void script_window_close_all(const void *owner)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].owner == owner) {
            script_window_close(owner, i + 1);
        }
    }
}

int script_window_keys(const void *owner, int id, const char *names[SCRIPT_WINDOW_MAX_KEYS])
{
    script_window *w = find(owner, id);
    if (w == NULL || w->closed_by_user) {
        return -1;
    }
    int n = w->key_n;
    for (int i = 0; i < n; i++) {
        names[i] = w->keys[i]; // (valid until the next key arrives: the caller copies them at once)
    }
    w->key_n = 0;
    return n;
}

bool script_window_save(const void *owner, int id, const char *path)
{
    script_window *w = find(owner, id);
    if (w == NULL) {
        return false;
    }
    uint8_t *rgb = malloc((size_t)w->width * (size_t)w->height * 3);
    if (rgb == NULL) {
        return false;
    }
    for (size_t i = 0; i < (size_t)w->width * (size_t)w->height; i++) {
        rgb[i * 3] = (uint8_t)(w->pixels[i] >> 16);
        rgb[i * 3 + 1] = (uint8_t)(w->pixels[i] >> 8);
        rgb[i * 3 + 2] = (uint8_t)w->pixels[i];
    }
    bool ok = png_write_rgb(path, rgb, w->width, w->height);
    free(rgb);
    return ok;
}

bool script_window_mouse(const void *owner, int id, int *x, int *y, unsigned *buttons)
{
    script_window *w = find(owner, id);
    if (w == NULL || w->closed_by_user || !w->mouse_in) {
        return false;
    }
    *x = w->mouse_x;
    *y = w->mouse_y;
    *buttons = w->mouse_buttons;
    return true;
}

bool script_window_event(const void *sdl_event)
{
#ifdef ONE_HAVE_SDL2
    const SDL_Event *event = (const SDL_Event *)sdl_event;
    Uint32 id;
    switch (event->type) {
    case SDL_WINDOWEVENT: id = event->window.windowID; break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: id = event->key.windowID; break;
    case SDL_TEXTEDITING: id = event->edit.windowID; break;
    case SDL_TEXTINPUT: id = event->text.windowID; break;
    case SDL_MOUSEMOTION: id = event->motion.windowID; break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: id = event->button.windowID; break;
    case SDL_MOUSEWHEEL: id = event->wheel.windowID; break;
    default: return false;
    }
    for (int i = 0; i < MAX_WINDOWS; i++) {
        script_window *w = &windows[i];
        if (w->owner == NULL || w->window == NULL || SDL_GetWindowID(w->window) != id) {
            continue;
        }
        if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_CLOSE) {
            destroy_window(w); // (the picture stays until the script closes it; one.windowKeys() returns nil)
            w->closed_by_user = true;
        } else if (event->type == SDL_WINDOWEVENT && (event->window.event == SDL_WINDOWEVENT_EXPOSED ||
                                                      event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
            script_window_show(w->owner, i + 1);
        } else if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_LEAVE) {
            w->mouse_in = false;
        } else if (event->type == SDL_MOUSEMOTION) {
            // (in the picture's pixels already: SDL converts them for a renderer with a logical size)
            int x = event->motion.x, y = event->motion.y;
            w->mouse_in = x >= 0 && y >= 0 && x < w->width && y < w->height;
            w->mouse_x = x;
            w->mouse_y = y;
        } else if (event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP) {
            unsigned bit = event->button.button == SDL_BUTTON_LEFT     ? 1u
                           : event->button.button == SDL_BUTTON_MIDDLE ? 2u
                           : event->button.button == SDL_BUTTON_RIGHT  ? 4u
                                                                       : 0u;
            w->mouse_buttons =
                (event->type == SDL_MOUSEBUTTONDOWN) ? (w->mouse_buttons | bit) : (w->mouse_buttons & ~bit);
        } else if (event->type == SDL_KEYDOWN && event->key.repeat == 0 && w->key_n < SCRIPT_WINDOW_MAX_KEYS) {
            snprintf(w->keys[w->key_n++], sizeof(w->keys[0]), "%s", SDL_GetKeyName(event->key.keysym.sym));
        }
        return true; // (its keys are not the game's)
    }
#else
    (void)sdl_event;
#endif
    return false;
}
