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
#include "audio.h"
#include "settings.h"
#include <stdio.h>
#include <math.h>

#define PI 3.14159265358979323846
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef ONE_HAVE_SDL2
#include <SDL.h>
#endif

#define DEFAULT_VOLUME      50
#define DEFAULT_SAMPLE_RATE 48000
#define DEFAULT_LATENCY     50  // ms
#define BATCH               256 // samples handed to the device at a time
#define MAX_RATE_DELTA      0.005

static bool enabled = true;
static int volume = DEFAULT_VOLUME;
static int sample_rate = DEFAULT_SAMPLE_RATE;
static int latency = DEFAULT_LATENCY;
static bool lowpass = true; // audio.lowpass: the NES's 14 kHz low-pass (Mesen has none)
static bool raw = false;    // audio.filters = none: the mixer's level as it is (tests that fit levels to a recording)

#define BUFFER_MAX 4096 // libretro: samples kept for the host (a frame is about 800; the rest is dropped)
static bool buffer_on = false;
static int16_t buffer[BUFFER_MAX];
static size_t buffer_count = 0;

static FILE *wav = NULL;
static uint32_t wav_samples = 0;
static double samples_per_cycle = 0.0;

// The filters of the NES's audio path (nesdev wiki "APU Mixer"): two first-order high-passes at 90 Hz and
// 440 Hz, then a first-order low-pass at 14 kHz.
static double hp90_k, hp440_k, lp14k_k;
static double hp90_x, hp90_y, hp440_x, hp440_y, lp_y;

#ifdef ONE_HAVE_SDL2
static SDL_AudioDeviceID device = 0;
static int16_t batch[BATCH];
static int batch_count = 0;
#endif

// --- settings (settings.h) ------------------------------------------------------------------------------------

static void update_lowpass(void)
{
    if (sample_rate > 0) {
        double dt = 1.0 / sample_rate;
        double rc14k = 1.0 / (2.0 * PI * 14000.0);
        lp14k_k = lowpass ? dt / (rc14k + dt) : 1.0; // 1.0: the low-pass passes its input through unchanged
    }
}

static bool apply_audio(const setting *s, const char *value, char *error, size_t error_size)
{
    if (error_size > 0) { // (never fails)
        error[0] = '\0';
    }
    bool on = false;
    switch (s->id) {
    case 0: settings_parse_bool(value, &enabled); break; // (SETTING_RESTART: read by audio_init())
    case 1: volume = atoi(value); break;
    case 2: sample_rate = atoi(value); break; // (SETTING_RESTART)
    case 3: latency = atoi(value); break;
    case 5: raw = (0 == strcmp(value, "none")); break;
    case 4:
        settings_parse_bool(value, &on);
        lowpass = on;
        update_lowpass();
        break;
    default: break;
    }
    return true;
}

static const char *const sample_rates[] = {"22050", "32000", "44100", "48000", "96000", NULL};
static const char *const filter_choices[] = {"nes", "none", NULL};

static const setting audio_table[] = {
    {"audio.enabled", SETTING_BOOL, "on", "Sound output", NULL, "off: no sound (a --wav file is still written)", 0, 0,
     NULL, SETTING_RESTART, apply_audio, 0},
    {"audio.volume", SETTING_INT, "50", "Volume", NULL,
     "0-100 (the speakers only; a --wav file is always at full scale)", 0, 100, NULL, 0, apply_audio, 1},
    {"audio.sample_rate", SETTING_CHOICE, "48000", "Sample rate (Hz)", NULL, "Hz", 0, 0, sample_rates, SETTING_RESTART,
     apply_audio, 2},
    {"audio.latency", SETTING_INT, "50", "Latency (ms)", NULL,
     "ms of sound queued ahead: lower reacts faster, higher crackles less", 10, 500, NULL, 0, apply_audio, 3},
    {"audio.lowpass", SETTING_BOOL, "on", "14 kHz treble cut", NULL,
     "the NES's 14 kHz treble cut (on = like a front-loading NES; off = like\n"
     "Mesen, slightly brighter); applies to --wav files too",
     0, 0, NULL, 0, apply_audio, 4},
    {"audio.filters", SETTING_CHOICE, "nes", "Output filters", NULL,
     "nes: the NES's high-passes (and the low-pass above); none: the mixer's level as it is\n"
     "(for fitting levels to a recording; applies to --wav files too)",
     0, 0, filter_choices, 0, apply_audio, 5},
};

const setting_group *audio_settings(void)
{
    static const setting_group group = {"sound", "Sound", NULL, audio_table,
                                        (int)(sizeof(audio_table) / sizeof(audio_table[0]))};
    return &group;
}

static void put_le(FILE *f, uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; i++) {
        fputc((int)((value >> (8 * i)) & 0xFF), f);
    }
}

static void wav_header(FILE *f, uint32_t samples)
{
    fwrite("RIFF", 1, 4, f);
    put_le(f, 36 + samples * 2, 4);
    fwrite("WAVEfmt ", 1, 8, f);
    put_le(f, 16, 4); // fmt chunk size
    put_le(f, 1, 2);  // PCM
    put_le(f, 1, 2);  // mono
    put_le(f, (uint32_t)sample_rate, 4);
    put_le(f, (uint32_t)sample_rate * 2, 4); // bytes per second
    put_le(f, 2, 2);                         // bytes per frame
    put_le(f, 16, 2);                        // bits per sample
    fwrite("data", 1, 4, f);
    put_le(f, samples * 2, 4);
}

bool audio_set_wav(const char *path)
{
    wav = fopen(path, "wb");
    if (wav == NULL) {
        return false;
    }
    wav_samples = 0;
    return true;
}

void audio_init(bool headless)
{
    double nominal = sample_rate * AUDIO_CPU_HZ_DEN / AUDIO_CPU_HZ_NUM;
    samples_per_cycle = nominal;
    double dt = 1.0 / sample_rate;
    double rc90 = 1.0 / (2.0 * PI * 90.0);
    double rc440 = 1.0 / (2.0 * PI * 440.0);
    hp90_k = rc90 / (rc90 + dt);
    hp440_k = rc440 / (rc440 + dt);
    update_lowpass();
    hp90_x = hp90_y = hp440_x = hp440_y = lp_y = 0.0;
    if (wav != NULL) {
        wav_header(wav, 0); // the sizes are filled in by audio_shutdown()
    }
#ifdef ONE_HAVE_SDL2
    if (headless || !enabled) {
        return;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        LOG_WARN(AUDIO, "no sound (%s)", SDL_GetError());
        return;
    }
    SDL_AudioSpec want;
    SDL_AudioSpec have;
    SDL_zero(want);
    want.freq = sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = BATCH;
    want.callback = NULL; // SDL_QueueAudio()
    device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (device == 0) {
        LOG_WARN(AUDIO, "cannot open the sound device (%s)", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }
    batch_count = 0;
    SDL_PauseAudioDevice(device, 0);
#else
    (void)headless;
#endif
}

void audio_shutdown(void)
{
#ifdef ONE_HAVE_SDL2
    if (device != 0) {
        SDL_CloseAudioDevice(device);
        device = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
#endif
    if (wav != NULL) {
        fseek(wav, 0, SEEK_SET);
        wav_header(wav, wav_samples);
        fclose(wav);
        wav = NULL;
    }
}

void audio_pause(bool pause)
{
#ifdef ONE_HAVE_SDL2
    if (device != 0) {
        SDL_PauseAudioDevice(device, pause ? 1 : 0);
        if (!pause) {
            SDL_ClearQueuedAudio(device); // (what was queued before the pause would play late)
            batch_count = 0;
        }
    }
#else
    (void)pause;
#endif
}

void audio_set_buffer(bool on)
{
    buffer_on = on;
    buffer_count = 0;
}

size_t audio_take_buffer(const int16_t **samples)
{
    *samples = buffer;
    size_t n = buffer_count;
    buffer_count = 0;
    return n;
}

bool audio_active(void)
{
    if (buffer_on) {
        return true;
    }
#ifdef ONE_HAVE_SDL2
    if (device != 0) {
        return true;
    }
#endif
    return wav != NULL;
}

double audio_samples_per_cycle(void)
{
    return samples_per_cycle;
}

static int16_t to_pcm(double value)
{
    double scaled = value * 32767.0;
    if (scaled > 32767.0) {
        return 32767;
    }
    if (scaled < -32768.0) {
        return -32768;
    }
    return (int16_t)lrint(scaled);
}

#ifdef ONE_HAVE_SDL2
// Dynamic rate control: run the sound a little faster when the device's queue is below audio.latency, a little
// slower when above, so that it neither runs dry nor grows, whatever the drift between the sound card's clock and
// the frame pacing. The pitch change is at most 0.5%, which nobody hears.
static void send_batch(void)
{
    int target = sample_rate * latency / 1000;
    int queued = (int)(SDL_GetQueuedAudioSize(device) / sizeof(int16_t));
    if (queued > 4 * target) {
        // far too much (running unthrottled, or after a pause): drop it rather than lag behind the picture
        SDL_ClearQueuedAudio(device);
        queued = 0;
    }
    SDL_QueueAudio(device, batch, (Uint32)(batch_count * (int)sizeof(int16_t)));
    batch_count = 0;
    double delta = MAX_RATE_DELTA * (double)(target - queued) / (double)target;
    if (delta > MAX_RATE_DELTA) {
        delta = MAX_RATE_DELTA;
    } else if (delta < -MAX_RATE_DELTA) {
        delta = -MAX_RATE_DELTA;
    }
    samples_per_cycle = (sample_rate * AUDIO_CPU_HZ_DEN / AUDIO_CPU_HZ_NUM) * (1.0 + delta);
}
#endif

void audio_sample(float level)
{
    double x = level;
    if (raw) {
        lp_y = x;
    } else {
        hp90_y = hp90_k * (hp90_y + x - hp90_x);
        hp90_x = x;
        hp440_y = hp440_k * (hp440_y + hp90_y - hp440_x);
        hp440_x = hp90_y;
        lp_y += lp14k_k * (hp440_y - lp_y);
    }

    if (buffer_on && buffer_count < BUFFER_MAX) {
        buffer[buffer_count++] = to_pcm(lp_y);
    }
    if (wav != NULL) {
        int16_t pcm = to_pcm(lp_y);
        put_le(wav, (uint16_t)pcm, 2);
        wav_samples++;
    }
#ifdef ONE_HAVE_SDL2
    if (device != 0) {
        batch[batch_count++] = to_pcm(lp_y * volume / 100.0);
        if (batch_count == BATCH) {
            send_batch();
        }
    }
#endif
}
