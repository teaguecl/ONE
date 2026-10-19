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
// Sound output: the APU's mixed level (rp2a03.c) goes through the NES's own filters and out to the speakers
// (SDL2), to a WAV file (--wav), or both. With neither, the APU does not generate samples at all.
#ifndef audio_h_defined
#define audio_h_defined

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "settings.h"

// The NTSC CPU clock: 39375000 / 22 Hz (21.477272 MHz / 12).
#define AUDIO_CPU_HZ_NUM 39375000.0
#define AUDIO_CPU_HZ_DEN 22.0

// one.cfg: audio.enabled, audio.volume, audio.sample_rate, audio.latency, audio.lowpass. CFG may be NULL (the
// defaults). Returns the number of mistakes (reported with LOG_WARN(CFG)).
const setting_group *audio_settings(void); // audio.* in one.cfg (settings.h)

// Also write everything played to PATH (16-bit mono PCM WAV at the sample rate). Call before audio_init().
// Returns false if PATH cannot be created.
bool audio_set_wav(const char *path);

// Opens the sound device (unless headless or audio.enabled = off) and/or the WAV file. Call after video_init().
void audio_init(bool headless);
void audio_shutdown(void);    // closes the device and finishes the WAV file
void audio_pause(bool pause); // the speakers only (the menu is open)

// The libretro core (libretro/): the samples go into a buffer that the host takes after each frame, as 16-bit mono
// at the sample rate (audio.sample_rate), whatever the volume setting says about the speakers. Call after audio_init().
void audio_set_buffer(bool on);
size_t audio_take_buffer(const int16_t **samples); // the samples since the last call (valid until the next frame)

// For rp2a03.c ------------------------------------------------------------------------------------------------
// Whether anyone listens: false = the APU skips sample generation.
bool audio_active(void);
// Output samples per CPU cycle, including the small speed-up or slow-down that keeps the sound device's queue at
// audio.latency (dynamic rate control; exactly sample_rate / CPU clock when only a WAV file is written).
double audio_samples_per_cycle(void);
// One output sample: the APU mixer's level (0.0 .. 1.0) averaged over the CPU cycles since the previous one.
void audio_sample(float level);

#endif
