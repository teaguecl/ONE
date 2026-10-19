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
// The Zapper. The photodiode sees a bright pixel under the aim point for a while after
// the beam has drawn it: light is on from the dot the beam draws the aim pixel until the start of the 21st scanline from
// the aim's (fitted to Mesen), provided that pixel is bright. There is no state of its own: the aim comes from
// input.c (a script, the mouse, a frontend), the picture from the PPU's frame buffer, the time from the PPU.
#include <stdbool.h>
#include <stdint.h>
#include "input.h"
#include "ppu.h"
#include "video.h"
#include "zapper.h"

#define FRAME_DOTS   (262u * 341u)
#define VBLANK_START (241u * 341u) // the aim is taken once a frame, here (Mesen reads its input at vblank)
// Fitted to Mesen's reads (tests/zapper/zapper_free_mesen.txt, 167,000 reads): the light is on from the dot the beam
// has drawn the aim pixel (dot x + 1 of the aim's scanline, 1..2 fit) until the start of scanline y + 21 (the end fits
// 7158..7163 dots after the start of the aim's scanline, whatever x is).
#define ZAPPER_START_DOTS  1u
#define ZAPPER_LIGHT_LINES 21u
// How bright the pixel under the aim must be: R + G + B of its palette color (video_palette_rgb) at least this: 85, as in Mesen's
// own source (Zapper: GetPixelBrightness() >= 85; a plain sum of the channels of the 2C02 palette, emphasis ignored). Fitted to
// Mesen's palette sweep (tests/zapper/sweep_all_mesen.txt, all 64 colors): dark up to 84 ($09), light from 87 ($0B); luma weights
// do not separate them. All 64 colors were recorded: the ten blacks, $09 and $0A are dark, the rest light.
#define ZAPPER_SUM_MIN 85u

static bool bright(uint8_t palette_index)
{
    uint32_t rgb = video_palette_rgb(palette_index);
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return r + g + b >= ZAPPER_SUM_MIN;
}

uint8_t zapper_read(void)
{
    static uint64_t aim_frame = UINT64_MAX; // the frame (counted from vblank) the aim was taken in
    static input_zapper_state aim;
    uint64_t now = ppu_dot_now();
    uint64_t frame = (now + FRAME_DOTS - VBLANK_START) / FRAME_DOTS;
    if (frame != aim_frame) {
        aim_frame = frame;
        if (!input_zapper_aim(&aim)) {
            aim = (input_zapper_state){false, 0, 0, false};
        }
    }
    uint8_t bits = aim.trigger ? 0x10u : 0u;
    bool light = false;
    if (aim.on_screen) {
        // dots since the start of the aim's scanline (a window that runs over the end of the frame goes on in the next)
        uint32_t since = (uint32_t)((now % FRAME_DOTS + FRAME_DOTS - (uint32_t)aim.y * 341u) % FRAME_DOTS);
        light = since >= (uint32_t)aim.x + ZAPPER_START_DOTS && since < ZAPPER_LIGHT_LINES * 341u &&
                bright(video_get_framebuffer()[aim.y * VIDEO_WIDTH + aim.x]);
    }
    return (uint8_t)(bits | (light ? 0u : 0x08u));
}
