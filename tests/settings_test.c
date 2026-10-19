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
// The settings table (settings.h): defaults, reading one.cfg, bad values, changing values, saving in place
// (ctest settings_table). Runs in a directory of its own (the test's working directory) with
// XDG_CONFIG_HOME/HOME pointing into it, so it never touches a real one.cfg.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "video.h"

static int failures = 0;

static void check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

static char *read_file(const char *path)
{
    static char text[65536];
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    text[n] = '\0';
    fclose(f);
    return text;
}

static int str_is(const char *a, const char *b)
{
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

// the same path, '\\' and '/' alike (Windows paths are made with backslashes)
static int path_is(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        if (*a != *b && !((*a == '/' || *a == '\\') && (*b == '/' || *b == '\\'))) {
            return 0;
        }
    }
    return *a == *b;
}

int main(void)
{
    char error[300];
    remove("one.cfg");
    remove("config/one/one.cfg");

    // no file anywhere: the defaults
    check("no settings file: no mistakes", settings_load(NULL) == 0);
    check("default audio.volume is 50", str_is(settings_get("audio.volume"), "50"));
    check("default joy1.a", str_is(settings_get("joy1.a"), "key:period, pad1:b"));
    check("default video.overscan is 8 (one)", str_is(settings_get("video.overscan"), "8"));
    check("default video.debugger_overscan is not set", str_is(settings_get("video.debugger_overscan"), ""));
    video_set_debugger(true); // what one-debugger does before loading its settings
    check("no settings file: no mistakes (overscan default 0)", settings_load(NULL) == 0);
    check("one-debugger's video.overscan default is 0", str_is(settings_get("video.overscan"), "0"));
    video_set_debugger(false);
    check("back to one's default", settings_load(NULL) == 0 && str_is(settings_get("video.overscan"), "8"));
    check("nothing changed after loading", !settings_changed());
    check("unknown key: NULL", settings_get("audio.nothing") == NULL);

    // --write-config output reads back without a single complaint
    FILE *out = fopen("defaults.cfg", "w");
    settings_write_defaults(out);
    fclose(out);
    check("--write-config output loads cleanly", settings_load("defaults.cfg") == 0);
    check("... and holds every default", str_is(settings_get("hotkey.menu"), "key:esc"));

    // a file with comments, good and bad values and an unknown key
    write_file("test.cfg", "# my settings\n"
                           "audio.volume = 80          # louder\n"
                           "audio.latency = 5000       # too much\n"
                           "audio.lowpass = maybe\n"
                           "video.scale = 2\n"
                           "\n"
                           "  log.level = debug   # indented\n"
                           "no.such.setting = 1\n"
                           "joy1.a = key:x\n"
                           "joy1.a = key:z   # the last one counts\n");
    int mistakes = settings_load("test.cfg");
    check("bad values and an unknown key are counted (3)", mistakes == 3);
    check("good value read", str_is(settings_get("audio.volume"), "80"));
    check("bad number keeps the default", str_is(settings_get("audio.latency"), "50"));
    check("bad on/off keeps the default", str_is(settings_get("audio.lowpass"), "on"));
    check("the last of two lines counts", str_is(settings_get("joy1.a"), "key:z"));
    check("settings_path is the file read", str_is(settings_path(), "test.cfg"));

    // changing values
    check("set a good value", settings_set_key("audio.volume", "65", error, sizeof(error)));
    check("... is the value now", str_is(settings_get("audio.volume"), "65"));
    check("... and counts as changed", settings_changed());
    check("a number out of range is refused", !settings_set_key("audio.volume", "101", error, sizeof(error)));
    check("... and changes nothing", str_is(settings_get("audio.volume"), "65"));
    check("a choice not in the list is refused", !settings_set_key("log.level", "loud", error, sizeof(error)));
    check("an unknown setting is refused", !settings_set_key("audio.nothing", "1", error, sizeof(error)));
#ifdef ONE_HAVE_SDL2
    check("a bad key name is refused", !settings_set_key("joy1.b", "key:nosuchkey", error, sizeof(error)));
    check("a bad hotkey (a gamepad button) is refused",
          !settings_set_key("hotkey.fps", "pad1:a", error, sizeof(error)));
#endif
    check("set a hotkey", settings_set_key("hotkey.fps", "key:ctrl+f", error, sizeof(error)));
    check("set a binding (not in the file)", settings_set_key("joy2.start", "key:enter", error, sizeof(error)));
    check("set log.level", settings_set_key("log.level", "warn", error, sizeof(error)));
    check("set joy1.a (twice in the file)", settings_set_key("joy1.a", "key:a", error, sizeof(error)));

    // saving: only the changed lines change, comments stay where they were, new ones are added at the end
    check("save", settings_save(error, sizeof(error)));
    check("nothing changed after saving", !settings_changed());
    const char *expected = "# my settings\n"
                           "audio.volume = 65          # louder\n"
                           "audio.latency = 5000       # too much\n"
                           "audio.lowpass = maybe\n"
                           "video.scale = 2\n"
                           "\n"
                           "  log.level = warn    # indented\n"
                           "no.such.setting = 1\n"
                           "joy1.a = key:x\n"
                           "joy1.a = key:a   # the last one counts\n"
                           "\n"
                           "# --- added by the menu ---\n"
                           "joy2.start = key:enter\n"
                           "hotkey.fps = key:ctrl+f\n";
    const char *saved = read_file("test.cfg");
    check("saved in place, comments kept", str_is(saved, expected));
    if (!str_is(saved, expected)) {
        printf("--- got:\n%s--- expected:\n%s---\n", saved ? saved : "(nothing)", expected);
    }
    check("the saved file reads back", settings_load("test.cfg") == 3 &&
                                           str_is(settings_get("joy2.start"), "key:enter") &&
                                           str_is(settings_get("hotkey.fps"), "key:ctrl+f"));

    // no file read: saving creates the user's file with every default, then the change
    settings_load(NULL);
    check("no file: the user's file is the target", path_is(settings_path(), "config/one/one.cfg"));
    settings_set_key("video.fullscreen", "on", error, sizeof(error));
    check("save creates the user's file", settings_save(error, sizeof(error)));
    settings_load("config/one/one.cfg");
    check("... with the change in it", str_is(settings_get("video.fullscreen"), "on"));
    check("... and the rest at their defaults", str_is(settings_get("audio.volume"), "50"));
    const char *created = read_file("config/one/one.cfg");
    check("... documented (comments from --write-config)", created != NULL &&
                                                               strstr(created, "# --- sound ---") != NULL &&
                                                               strstr(created, "video.fullscreen = on") != NULL);

    remove("defaults.cfg");
    remove("test.cfg");
    remove("config/one/one.cfg");
    printf("%s\n", failures == 0 ? "settings_test: all passed" : "settings_test: FAILED");
    return failures == 0 ? 0 : 1;
}
