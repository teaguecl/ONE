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
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef ONE_HAVE_SDL2
#include <SDL.h>
#endif
#include "log.h"
#include "cpu.h"
#include "input.h"
#include "video.h"
#include "savestate.h"
#include "settings.h"
#include "frontend.h"
#include "script_window.h"
#ifdef ONE_DEBUGGER
#include "debug_hooks.h"
#endif

// NES controller buttons, in the order the controller shifts them out (bit 0 first; one.cfg names: joyN.a ...)
static const char *const button_labels[INPUT_BUTTONS] = {"A", "B", "Select", "Start", "Up", "Down", "Left", "Right"};
enum { BTN_A, BTN_B, BTN_SELECT, BTN_START, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT };

// The defaults: what one.cfg says when it is written by --write-config
static const char *const default_bindings[INPUT_PORTS][INPUT_BUTTONS] = {
    {"key:period, pad1:b", "key:comma, pad1:a", "key:leftbracket, pad1:back", "key:rightbracket, pad1:start",
     "key:up, pad1:dpup, pad1:lefty-", "key:down, pad1:dpdown, pad1:lefty+", "key:left, pad1:dpleft, pad1:leftx-",
     "key:right, pad1:dpright, pad1:leftx+"},
    {"pad2:b", "pad2:a", "pad2:back", "pad2:start", "pad2:dpup, pad2:lefty-", "pad2:dpdown, pad2:lefty+",
     "pad2:dpleft, pad2:leftx-", "pad2:dpright, pad2:leftx+"},
};
#define DEFAULT_AXIS_THRESHOLD 50 // percent (input.axis_threshold)

// Hotkeys (game window): hotkey.NAME = keys, e.g. key:shift+f12. Chosen so that they clash with neither SoftICE's
// default function keys nor the debugger's (F1-F12, Shift+F3, Ctrl+F1/F2/F5-F7, Alt+F-keys, Ctrl+D).
enum {
    HOTKEY_MENU,
    HOTKEY_SCREENSHOT,
    HOTKEY_FPS,
    HOTKEY_FULLSCREEN,
    HOTKEY_STATE_SAVE,
    HOTKEY_STATE_LOAD,
    HOTKEY_SLOT_PREVIOUS,
    HOTKEY_SLOT_NEXT,
    HOTKEYS
};

// The menu (frontend.h): opened by hotkey.menu or the gamepad button menu.gamepad_button, at the end of the frame
static bool menu_requested = false;
static input_event_hook event_hook = NULL; // one-debugger's window (input_set_event_hook())
static void *event_hook_context = NULL;
static const char *const menu_buttons[] = {"guide", "back+start", "select+start", "leftstick", "rightstick",
                                           "none",  NULL};
enum { MENU_GUIDE, MENU_BACK_START, MENU_SELECT_START, MENU_LEFTSTICK, MENU_RIGHTSTICK };
#ifdef ONE_HAVE_SDL2
static bool select_start_held = false; // (select+start: the menu opens when both are down, once)
#endif
static int menu_button = 0; // index into menu_buttons

// Scripted input (-S): START is held from this CPU cycle for 2 frames, long enough for a game polling once per
// frame to see the press and the release.
#define SCRIPTED_PRESS_CYCLES (2 * 29781)
static bool scripted_start = false;
static uint64_t scripted_start_cycle = 0;

void input_set_scripted_start(uint64_t cpu_cycle)
{
    scripted_start = true;
    scripted_start_cycle = cpu_cycle;
}

// Scripted input (--input FILE): from each listed CPU cycle on, the buttons held on the controllers. The file is
// what regression/input_from_log.py recovers from a Mesen trace: "CYCLE BUTTONS [BUTTONS2]" per line, BUTTONS being
// names joined by '+' (a, b, select, start, up, down, left, right) or '-' for none; '#' starts a comment.
// A line "CYCLE zapper X Y [fire]" (X 0-255, Y 0-239 in NES pixels; "off" instead of X Y: aimed away from the
// screen; "fire": trigger pulled) sets the Zapper on port 2 from that cycle on; it is independent of the buttons.
typedef struct {
    uint64_t cycle;
    uint8_t buttons[INPUT_PORTS];
} scripted_change;
static scripted_change *script_changes = NULL; // in cycle order
static size_t script_count = 0;

typedef struct {
    uint64_t cycle;
    input_zapper_state state;
} scripted_zapper_change;
static scripted_zapper_change *zapper_changes = NULL; // in cycle order
static size_t zapper_count = 0;

bool input_parse_buttons(const char *text, uint8_t *buttons)
{
    static const char *const names[] = {"a", "b", "select", "start", "up", "down", "left", "right"};
    *buttons = 0;
    if (0 == strcmp(text, "-")) {
        return true;
    }
    while ('\0' != *text) {
        size_t len = strcspn(text, "+");
        int b = 0;
        while ((b < INPUT_BUTTONS) && !((strlen(names[b]) == len) && (0 == strncmp(text, names[b], len)))) {
            b++;
        }
        if (b == INPUT_BUTTONS) {
            return false;
        }
        *buttons |= (uint8_t)(1u << b);
        text += len + ((text[len] == '+') ? 1 : 0);
    }
    return true;
}

// The rest of a "zapper" line: "X Y [fire]" or "off [fire]"
static bool parse_zapper(const char *text, input_zapper_state *state)
{
    char word[4][32] = {"", "", "", ""};
    int fields = sscanf(text, "%31s %31s %31s %31s", word[0], word[1], word[2], word[3]);
    *state = (input_zapper_state){false, 0, 0, false};
    int next;
    if (fields >= 1 && 0 == strcmp(word[0], "off")) {
        next = 1;
    } else {
        char *end_x, *end_y;
        long x = strtol(word[0], &end_x, 10);
        long y = strtol(word[1], &end_y, 10);
        if (fields < 2 || '\0' != *end_x || '\0' != *end_y || x < 0 || x > 255 || y < 0 || y > 239) {
            return false;
        }
        state->on_screen = true;
        state->x = (int)x;
        state->y = (int)y;
        next = 2;
    }
    if (fields > next + 1) {
        return false;
    }
    if (fields == next + 1) {
        if (0 != strcmp(word[next], "fire")) {
            return false;
        }
        state->trigger = true;
    }
    return true;
}

bool input_load_script(const char *path)
{
    FILE *file = fopen(path, "r");
    if (NULL == file) {
        LOG_ERROR(INPUT, "cannot open the input script %s", path);
        return false;
    }
    free(script_changes);
    script_changes = NULL;
    script_count = 0;
    free(zapper_changes);
    zapper_changes = NULL;
    zapper_count = 0;
    size_t capacity = 0, zapper_capacity = 0;
    char line[256];
    int number = 0;
    bool ok = true;
    while (ok && (NULL != fgets(line, sizeof(line), file))) {
        number++;
        char *hash = strchr(line, '#');
        if (NULL != hash) {
            *hash = '\0';
        }
        unsigned long long cycle;
        char first[64], second[64] = "-";
        int fields = sscanf(line, "%llu %63s %63s", &cycle, first, second);
        if (fields <= 0) {
            continue; // (empty or comment)
        }
        if ((fields >= 2) && (0 == strcmp(first, "zapper"))) {
            const char *rest = strstr(line, "zapper") + strlen("zapper");
            scripted_zapper_change zc = {cycle, {false, 0, 0, false}};
            if (!parse_zapper(rest, &zc.state) ||
                ((zapper_count > 0) && (cycle < zapper_changes[zapper_count - 1].cycle))) {
                LOG_ERROR(INPUT,
                          "%s:%d: expected 'CYCLE zapper X Y [fire]' or 'CYCLE zapper off [fire]' in cycle order", path,
                          number);
                ok = false;
                break;
            }
            if (zapper_count == zapper_capacity) {
                zapper_capacity = (zapper_capacity == 0) ? 16 : zapper_capacity * 2;
                scripted_zapper_change *grown = realloc(zapper_changes, zapper_capacity * sizeof(*grown));
                if (NULL == grown) {
                    ok = false;
                    break;
                }
                zapper_changes = grown;
            }
            zapper_changes[zapper_count++] = zc;
            continue;
        }
        scripted_change change = {cycle, {0, 0}};
        if ((fields < 2) || !input_parse_buttons(first, &change.buttons[0]) ||
            !input_parse_buttons(second, &change.buttons[1]) ||
            ((script_count > 0) && (cycle < script_changes[script_count - 1].cycle))) {
            LOG_ERROR(INPUT, "%s:%d: expected 'CYCLE BUTTONS [BUTTONS2]' in cycle order", path, number);
            ok = false;
            break;
        }
        if (script_count == capacity) {
            capacity = (capacity == 0) ? 64 : capacity * 2;
            scripted_change *grown = realloc(script_changes, capacity * sizeof(*grown));
            if (NULL == grown) {
                ok = false;
                break;
            }
            script_changes = grown;
        }
        script_changes[script_count++] = change;
    }
    fclose(file);
    if (!ok) {
        free(script_changes);
        script_changes = NULL;
        script_count = 0;
        free(zapper_changes);
        zapper_changes = NULL;
        zapper_count = 0;
    }
    return ok;
}

bool input_scripted_zapper(input_zapper_state *state)
{
    if (0 == zapper_count) {
        return false;
    }
    uint64_t now = cpu_get_cycle();
    size_t lo = 0, hi = zapper_count; // binary search: the first change after now
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (zapper_changes[mid].cycle <= now) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *state = (lo == 0) ? (input_zapper_state){false, 0, 0, false} : zapper_changes[lo - 1].state;
    return true;
}

// The scripted buttons of a port at this CPU cycle (the last change at or before it)
static uint8_t scripted_buttons(int port)
{
    uint64_t now = cpu_get_cycle();
    size_t lo = 0, hi = script_count; // binary search: the first change after now
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (script_changes[mid].cycle <= now) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (lo == 0) ? 0 : script_changes[lo - 1].buttons[port];
}

static bool four_score = false;      // input.four_score (also without SDL2: libretro)
static bool zapper_setting = false;  // input.zapper: a Zapper on port 2, aimed with the mouse
static bool zapper_override = false; // a frontend sets the aim (libretro's light gun)
static input_zapper_state zapper_override_state;
static bool script_zapper_set = false; // a Lua script's aim for this frame (emu.setInput on port 2 with x / y)
static input_zapper_state script_zapper;
static uint64_t zapper_trigger_until =
    0; // CPU cycle: the trigger is held until then (the debugger's 'zapper ... fire')

void input_set_zapper_frame(const input_zapper_state *state)
{
    script_zapper_set = (state != NULL);
    if (state != NULL) {
        script_zapper = *state;
    }
}

void input_zapper_pull_trigger(uint64_t until_cycle)
{
    zapper_trigger_until = until_cycle;
}

void input_set_zapper(const input_zapper_state *state)
{
    zapper_override = (state != NULL);
    if (state != NULL) {
        zapper_override_state = *state;
    }
}

bool input_zapper_present(void)
{
    return zapper_setting || zapper_override || script_zapper_set || (zapper_count > 0);
}

// Where the Zapper points: a script's aim, else a frontend's, else the mouse over the game window (left button = trigger)
bool input_zapper_aim(input_zapper_state *state)
{
    if (input_scripted_zapper(state)) {
        return true;
    }
    if (script_zapper_set) {
        *state = script_zapper;
        return true;
    }
    if (zapper_override) {
        *state = zapper_override_state;
        state->trigger = state->trigger || (cpu_get_cycle() < zapper_trigger_until);
        return true;
    }
    *state = (input_zapper_state){false, 0, 0, false};
#ifdef ONE_HAVE_SDL2
    int x, y;
    unsigned buttons;
    if (video_mouse(&x, &y, &buttons)) {
        state->on_screen = (x >= 0 && x < VIDEO_WIDTH && y >= 0 && y < VIDEO_HEIGHT);
        state->x = x;
        state->y = y;
        state->trigger = (buttons & 1u) != 0;
    }
#endif
    return true;
}

#ifdef ONE_HAVE_SDL2

// --- bindings -------------------------------------------------------------------------------------------------

typedef enum {
    BIND_KEY,        // index = SDL scancode
    BIND_PAD_BUTTON, // index = SDL_GameControllerButton (named, needs a pad SDL recognises)
    BIND_PAD_AXIS,   // index = SDL_GameControllerAxis, dir = +1 / -1
    BIND_JOY_BUTTON, // index = raw button number
    BIND_JOY_AXIS,   // index = raw axis number, dir = +1 / -1
    BIND_JOY_HAT,    // index = raw hat number, hat = SDL_HAT_UP/DOWN/LEFT/RIGHT
} bind_kind;

typedef struct {
    bind_kind kind;
    int pad; // 0-based gamepad slot (padN: N - 1)
    int index;
    int dir;
    uint8_t hat;
} binding;

#define MAX_BINDINGS 8
#define MAX_PADS     4

static binding bindings[INPUT_PORTS][INPUT_BUTTONS][MAX_BINDINGS];
static int binding_count[INPUT_PORTS][INPUT_BUTTONS];
static int16_t axis_threshold = (int16_t)(32767 * DEFAULT_AXIS_THRESHOLD / 100);
static bool opposite_directions = false;

// Opened gamepads, by slot: pad1 is the first connected
static struct {
    SDL_Joystick *joystick;         // NULL: slot free
    SDL_GameController *controller; // NULL: SDL does not know the pad (only raw inputs work)
    SDL_JoystickID id;
} pads[MAX_PADS];
static bool gamepads_enabled = false;

// key:NAME -> SDL scancode. Friendly names first, then SDL's own names ('_' for spaces)
static SDL_Scancode key_from_name(const char *name)
{
    static const struct {
        const char *alias;
        const char *sdl;
    } aliases[] = {
        {"enter", "Return"},      {"esc", "Escape"},
        {"lshift", "Left Shift"}, {"rshift", "Right Shift"},
        {"lctrl", "Left Ctrl"},   {"rctrl", "Right Ctrl"},
        {"lalt", "Left Alt"},     {"ralt", "Right Alt"},
        {"comma", ","},           {"period", "."},
        {"slash", "/"},           {"backslash", "\\"},
        {"semicolon", ";"},       {"apostrophe", "'"},
        {"quote", "'"},           {"leftbracket", "["},
        {"rightbracket", "]"},    {"minus", "-"},
        {"equals", "="},          {"grave", "`"},
        {"backquote", "`"},       {"kpenter", "Keypad Enter"},
        {"kpplus", "Keypad +"},   {"kpminus", "Keypad -"},
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (SDL_strcasecmp(name, aliases[i].alias) == 0) {
            return SDL_GetScancodeFromName(aliases[i].sdl);
        }
    }
    char sdl_name[64];
    if (strncmp(name, "kp", 2) == 0 && isdigit((unsigned char)name[2]) && name[3] == '\0') {
        snprintf(sdl_name, sizeof(sdl_name), "Keypad %c", name[2]);
    } else {
        snprintf(sdl_name, sizeof(sdl_name), "%s", name);
        for (char *c = sdl_name; *c != '\0'; c++) {
            if (*c == '_') {
                *c = ' ';
            }
        }
    }
    return SDL_GetScancodeFromName(sdl_name);
}

// padN:NAME. Returns false (with a message in ERROR) if NAME is not a button, axis or hat name.
static bool pad_input_from_name(const char *name, binding *b, char *error, size_t error_size)
{
    static const struct {
        const char *alias;
        const char *sdl;
    } aliases[] = {
        {"dpad_up", "dpup"},  {"dpad_down", "dpdown"}, {"dpad_left", "dpleft"}, {"dpad_right", "dpright"},
        {"select", "back"},   {"l1", "leftshoulder"},  {"r1", "rightshoulder"}, {"l3", "leftstick"},
        {"r3", "rightstick"}, {"lb", "leftshoulder"},  {"rb", "rightshoulder"},
    };
    const char *sdl = name;
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (strcmp(name, aliases[i].alias) == 0) {
            sdl = aliases[i].sdl;
        }
    }
    SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(sdl);
    if (button != SDL_CONTROLLER_BUTTON_INVALID) {
        b->kind = BIND_PAD_BUTTON;
        b->index = button;
        return true;
    }
    size_t len = strlen(name);
    if (len >= 2 && (name[len - 1] == '+' || name[len - 1] == '-')) {
        char base[32];
        snprintf(base, sizeof(base), "%.*s", (int)(len - 1), name);
        b->dir = (name[len - 1] == '+') ? 1 : -1;
        SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(base);
        if (axis != SDL_CONTROLLER_AXIS_INVALID) {
            b->kind = BIND_PAD_AXIS;
            b->index = axis;
            return true;
        }
        int number = 0;
        if (sscanf(base, "axis%d", &number) == 1 && number >= 0) {
            b->kind = BIND_JOY_AXIS;
            b->index = number;
            return true;
        }
    }
    int number = 0;
    char direction[8] = "";
    char extra = 0;
    if (sscanf(name, "button%d%c", &number, &extra) == 1 && number >= 0) {
        b->kind = BIND_JOY_BUTTON;
        b->index = number;
        return true;
    }
    if (sscanf(name, "hat%d_%7s", &number, direction) == 2 && number >= 0) {
        static const struct {
            const char *name;
            uint8_t mask;
        } hats[] = {{"up", SDL_HAT_UP}, {"down", SDL_HAT_DOWN}, {"left", SDL_HAT_LEFT}, {"right", SDL_HAT_RIGHT}};
        for (size_t i = 0; i < sizeof(hats) / sizeof(hats[0]); i++) {
            if (strcmp(direction, hats[i].name) == 0) {
                b->kind = BIND_JOY_HAT;
                b->index = number;
                b->hat = hats[i].mask;
                return true;
            }
        }
    }
    snprintf(error, error_size, "unknown gamepad input '%s' (one --list-gamepads shows the names)", name);
    return false;
}

// One input: "key:NAME" or "padN:NAME"
static bool parse_binding(const char *text, binding *b, char *error, size_t error_size)
{
    memset(b, 0, sizeof(*b));
    if (strncmp(text, "key:", 4) == 0) {
        SDL_Scancode sc = key_from_name(text + 4);
        if (sc == SDL_SCANCODE_UNKNOWN) {
            snprintf(error, error_size, "unknown key '%s'", text + 4);
            return false;
        }
        b->kind = BIND_KEY;
        b->index = sc;
        return true;
    }
    int pad = 0;
    int consumed = 0;
    if (sscanf(text, "pad%d:%n", &pad, &consumed) == 1 && consumed > 0) {
        if (pad < 1 || pad > MAX_PADS) {
            snprintf(error, error_size, "gamepad number must be 1-%d: '%s'", MAX_PADS, text);
            return false;
        }
        b->pad = pad - 1;
        return pad_input_from_name(text + consumed, b, error, error_size);
    }
    snprintf(error, error_size, "'%s' is not key:NAME or padN:NAME", text);
    return false;
}

// --- hotkeys ------------------------------------------------------------------------------------------------

#define HOTKEY_SHIFT    1
#define HOTKEY_CTRL     2
#define HOTKEY_ALT      4
#define MAX_HOTKEY_KEYS 4

typedef struct {
    SDL_Scancode key;
    int modifiers; // HOTKEY_SHIFT | HOTKEY_CTRL | HOTKEY_ALT, all of which must be held (and no others)
} hotkey;

static hotkey hotkeys[HOTKEYS][MAX_HOTKEY_KEYS];
static int hotkey_count[HOTKEYS];

// "key:shift+f12, key:ctrl+alt+s" (or "none") -> the keys of one hotkey. Returns false on an error.
static bool parse_hotkey_list(const char *value, int which, char *error, size_t error_size)
{
    hotkey parsed[MAX_HOTKEY_KEYS];
    int count = 0;
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", value);
    for (char *item = copy, *next = NULL; item != NULL; item = next) {
        next = strchr(item, ',');
        if (next != NULL) {
            *next++ = '\0';
        }
        while (isspace((unsigned char)*item)) {
            item++;
        }
        char *end = item + strlen(item);
        while (end > item && isspace((unsigned char)end[-1])) {
            *--end = '\0';
        }
        for (char *c = item; *c != '\0'; c++) {
            *c = (char)tolower((unsigned char)*c);
        }
        if (*item == '\0' || strcmp(item, "none") == 0) {
            continue;
        }
        if (strncmp(item, "key:", 4) != 0) {
            snprintf(error, error_size, "'%s': hotkeys are keys (key:NAME, e.g. key:shift+f12)", item);
            return false;
        }
        if (count == MAX_HOTKEY_KEYS) {
            snprintf(error, error_size, "at most %d keys per hotkey", MAX_HOTKEY_KEYS);
            return false;
        }
        char *name = item + 4;
        int modifiers = 0;
        for (;;) {
            if (strncmp(name, "shift+", 6) == 0) {
                modifiers |= HOTKEY_SHIFT;
                name += 6;
            } else if (strncmp(name, "ctrl+", 5) == 0) {
                modifiers |= HOTKEY_CTRL;
                name += 5;
            } else if (strncmp(name, "alt+", 4) == 0) {
                modifiers |= HOTKEY_ALT;
                name += 4;
            } else {
                break;
            }
        }
        SDL_Scancode sc = key_from_name(name);
        if (sc == SDL_SCANCODE_UNKNOWN) {
            snprintf(error, error_size, "unknown key '%s'", name);
            return false;
        }
        parsed[count].key = sc;
        parsed[count].modifiers = modifiers;
        count++;
    }
    memcpy(hotkeys[which], parsed, sizeof(hotkey) * (size_t)count);
    hotkey_count[which] = count;
    return true;
}

static void hotkey_run(int which)
{
    if (which == HOTKEY_MENU) {
        menu_requested = fe_menu_available();
    } else if (which == HOTKEY_FULLSCREEN) {
        video_toggle_fullscreen();
    } else if (which == HOTKEY_SCREENSHOT) {
        char saved[1100];
        if (video_screenshot(NULL, saved, sizeof(saved))) {
            const char *name = strrchr(saved, '/');
            video_overlay("Screenshot %s", name != NULL ? name + 1 : saved);
            LOG_INFO(VIDEO, "Screenshot saved: %s", saved);
        } else {
            video_overlay("Screenshot failed");
            LOG_WARN(VIDEO, "Screenshot: cannot write %s", saved);
        }
    } else if (which == HOTKEY_FPS) {
        video_set_fps(!video_fps_shown());
        video_overlay("FPS counter %s", video_fps_shown() ? "on" : "off");
        LOG_INFO(VIDEO, "FPS counter %s", video_fps_shown() ? "on (in the window title)" : "off");
    } else if (which == HOTKEY_STATE_SAVE) {
        savestate_save_slot();
    } else if (which == HOTKEY_STATE_LOAD) {
        savestate_load_slot();
    } else if (which == HOTKEY_SLOT_PREVIOUS) {
        savestate_change_slot(-1);
    } else if (which == HOTKEY_SLOT_NEXT) {
        savestate_change_slot(+1);
    }
}

// A key went down in the game window: run the hotkey it belongs to. Returns false if it is not a hotkey.
static bool hotkey_key_down(const SDL_KeyboardEvent *key)
{
    int modifiers = ((key->keysym.mod & KMOD_SHIFT) ? HOTKEY_SHIFT : 0) |
                    ((key->keysym.mod & KMOD_CTRL) ? HOTKEY_CTRL : 0) | ((key->keysym.mod & KMOD_ALT) ? HOTKEY_ALT : 0);
    for (int which = 0; which < HOTKEYS; which++) {
        for (int i = 0; i < hotkey_count[which]; i++) {
            if (hotkeys[which][i].key == key->keysym.scancode && hotkeys[which][i].modifiers == modifiers) {
                if (key->repeat == 0) { // holding it down does not take a screenshot per auto-repeat
                    hotkey_run(which);
                }
                return true;
            }
        }
    }
    return false;
}

// "key:period, pad1:b" -> the bindings of one NES button. Returns false (keeping the old bindings) on an error.
static bool parse_binding_list(const char *value, int port, int button, char *error, size_t error_size)
{
    binding parsed[MAX_BINDINGS];
    int count = 0;
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", value);
    for (char *item = copy, *next = NULL; item != NULL; item = next) {
        next = strchr(item, ','); // (not strtok_r: MSVC has no such function)
        if (next != NULL) {
            *next++ = '\0';
        }
        while (isspace((unsigned char)*item)) {
            item++;
        }
        char *end = item + strlen(item);
        while (end > item && isspace((unsigned char)end[-1])) {
            *--end = '\0';
        }
        for (char *c = item; *c != '\0'; c++) {
            *c = (char)tolower((unsigned char)*c);
        }
        if (*item == '\0' || strcmp(item, "none") == 0) {
            continue;
        }
        if (count == MAX_BINDINGS) {
            snprintf(error, error_size, "at most %d inputs per button", MAX_BINDINGS);
            return false;
        }
        if (!parse_binding(item, &parsed[count], error, error_size)) {
            return false;
        }
        count++;
    }
    memcpy(bindings[port][button], parsed, sizeof(binding) * (size_t)count);
    binding_count[port][button] = count;
    return true;
}

static bool binding_pressed(const binding *b, const Uint8 *keys)
{
    if (b->kind == BIND_KEY) {
        return keys != NULL && keys[b->index];
    }
    SDL_Joystick *joystick = pads[b->pad].joystick;
    SDL_GameController *controller = pads[b->pad].controller;
    if (joystick == NULL) {
        return false;
    }
    switch (b->kind) {
    case BIND_PAD_BUTTON:
        return controller != NULL && SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)b->index);
    case BIND_PAD_AXIS:
        return controller != NULL &&
               SDL_GameControllerGetAxis(controller, (SDL_GameControllerAxis)b->index) * b->dir > axis_threshold;
    case BIND_JOY_BUTTON: return SDL_JoystickGetButton(joystick, b->index) != 0;
    case BIND_JOY_AXIS: return SDL_JoystickGetAxis(joystick, b->index) * b->dir > axis_threshold;
    case BIND_JOY_HAT: return (SDL_JoystickGetHat(joystick, b->index) & b->hat) != 0;
    default: return false;
    }
}

static uint8_t read_port(int port)
{
    if (!SDL_WasInit(SDL_INIT_EVENTS)) {
        return 0;
    }
    // the keyboard plays the game only while the game window has the focus (not while one-debugger's window has it)
    SDL_Window *focus = SDL_GetKeyboardFocus();
    const Uint8 *keys = (focus == NULL || focus == fe_sdl_window()) ? SDL_GetKeyboardState(NULL) : NULL;
    uint8_t state = 0;
    for (int button = 0; button < INPUT_BUTTONS; button++) {
        for (int i = 0; i < binding_count[port][button]; i++) {
            if (binding_pressed(&bindings[port][button][i], keys)) {
                state |= (uint8_t)(1u << button);
                break;
            }
        }
    }
    if (!opposite_directions) {
        // a real controller cannot press both: some games misbehave when they see it
        const uint8_t up_down = (1u << BTN_UP) | (1u << BTN_DOWN);
        const uint8_t left_right = (1u << BTN_LEFT) | (1u << BTN_RIGHT);
        if ((state & up_down) == up_down) {
            state &= (uint8_t)~up_down;
        }
        if ((state & left_right) == left_right) {
            state &= (uint8_t)~left_right;
        }
    }
    return state;
}

// --- gamepads ------------------------------------------------------------------------------------------------

static void pad_added(int device_index)
{
    int slot = 0;
    while (slot < MAX_PADS && pads[slot].joystick != NULL) {
        slot++;
    }
    if (slot == MAX_PADS) {
        LOG_WARN(INPUT, "more than %d gamepads, ignoring '%s'", MAX_PADS, SDL_JoystickNameForIndex(device_index));
        return;
    }
    SDL_GameController *controller = NULL;
    SDL_Joystick *joystick = NULL;
    if (SDL_IsGameController(device_index)) {
        controller = SDL_GameControllerOpen(device_index);
        joystick = controller ? SDL_GameControllerGetJoystick(controller) : NULL;
    } else {
        joystick = SDL_JoystickOpen(device_index);
    }
    if (joystick == NULL) {
        LOG_WARN(INPUT, "cannot open gamepad %d (%s)", device_index, SDL_GetError());
        return;
    }
    SDL_JoystickID id = SDL_JoystickInstanceID(joystick);
    for (int i = 0; i < MAX_PADS; i++) {
        if (pads[i].joystick != NULL && pads[i].id == id) { // already open (added twice)
            if (controller) {
                SDL_GameControllerClose(controller);
            } else {
                SDL_JoystickClose(joystick);
            }
            return;
        }
    }
    pads[slot].joystick = joystick;
    pads[slot].controller = controller;
    pads[slot].id = id;
    LOG_INFO(INPUT, "pad%d connected: %s%s", slot + 1, SDL_JoystickName(joystick),
             controller ? "" : " (not in SDL's controller database: use raw names, see one --list-gamepads)");
}

static void pad_removed(SDL_JoystickID id)
{
    for (int i = 0; i < MAX_PADS; i++) {
        if (pads[i].joystick != NULL && pads[i].id == id) {
            LOG_INFO(INPUT, "pad%d disconnected", i + 1);
            if (pads[i].controller) {
                SDL_GameControllerClose(pads[i].controller);
            } else {
                SDL_JoystickClose(pads[i].joystick);
            }
            memset(&pads[i], 0, sizeof(pads[i]));
        }
    }
}

// The game window's close button. With other windows open (a script's, the debugger's) SDL sends no SDL_QUIT for it,
// only this; with the game window alone it sends both: the SDL_QUIT right after it is then the same request.
static bool quit_from_close = false;

static void input_handle_event(SDL_Event *event)
{
    SDL_Window *game = (SDL_Window *)fe_sdl_window();
    SDL_Event quit; // (the close button, handled as SDL_QUIT)
    memset(&quit, 0, sizeof(quit));
    quit.type = SDL_QUIT;
    if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_CLOSE) {
        if (game == NULL || event->window.windowID != SDL_GetWindowID(game)) {
            return;
        }
        quit_from_close = true;
        event = &quit;
    } else if (event->type == SDL_QUIT && quit_from_close) {
        quit_from_close = false;
        return;
    }
    if ((event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) && game != NULL && event->key.windowID != 0 &&
        event->key.windowID != SDL_GetWindowID(game)) {
        return; // (another window's keys are not the game's hotkeys; 0: no window -- pushed, as by input_test.c)
    }
    switch (event->type) {
    case SDL_QUIT:
#ifdef ONE_DEBUGGER
        if (dbg_key_hook) {
            dbg_key_hook(DBG_KEY_QUIT, false); // debugger build: the debugger quits (and closes the window)
            break;
        }
#endif
        LOG_DEBUG(INPUT, "window closed");
        cpu_request_stop(); // window closed: stop emulation at the next instruction boundary
        break;
    case SDL_JOYDEVICEADDED:
        if (gamepads_enabled) {
            pad_added(event->jdevice.which);
        }
        break;
    case SDL_JOYDEVICEREMOVED:
        if (gamepads_enabled) {
            pad_removed(event->jdevice.which);
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_JOYBUTTONDOWN:
    case SDL_JOYHATMOTION:
        if (fe_is_menu_button(event)) {
            menu_requested = fe_menu_available();
        }
        break;
    case SDL_CONTROLLERBUTTONUP:
    case SDL_JOYBUTTONUP:
        if (menu_button == MENU_SELECT_START) { // (let go: the next press of both opens the menu again)
            const uint8_t both = (1u << BTN_SELECT) | (1u << BTN_START);
            select_start_held = (read_port(0) & both) == both;
        }
        break;
    case SDL_KEYDOWN:
        if (hotkey_key_down(&event->key)) {
            break; // (before the debugger's keys: Shift+F12 is the screenshot, not F12)
        }
#ifdef ONE_DEBUGGER
        // Debugger build: the keys that stop the game drive the debugger from the game window too: F5 go/stop,
        // F8 trace, F10 step over, F12 step to return, Ctrl+D break, Alt+X quit (the other function keys are the
        // game window's own: F1/F2 save states, ...)
        if (dbg_key_hook) {
            SDL_Keycode sym = event->key.keysym.sym;
            Uint16 mod = event->key.keysym.mod & (KMOD_SHIFT | KMOD_CTRL | KMOD_ALT);
            bool repeat = event->key.repeat != 0;
            SDL_Scancode sc = event->key.keysym.scancode;
            if (mod == 0 &&
                (sc == SDL_SCANCODE_F5 || sc == SDL_SCANCODE_F8 || sc == SDL_SCANCODE_F10 || sc == SDL_SCANCODE_F12)) {
                dbg_key_hook(1 + (int)(sc - SDL_SCANCODE_F1), repeat);
            } else if (sym == SDLK_d && (mod & KMOD_CTRL) && !(mod & (KMOD_SHIFT | KMOD_ALT))) {
                dbg_key_hook(DBG_KEY_CTRL_D, repeat);
            } else if (sym == SDLK_x && (mod & KMOD_ALT) && !(mod & (KMOD_SHIFT | KMOD_CTRL))) {
                dbg_key_hook(DBG_KEY_ALT_X, repeat);
            }
        }
#endif
        break;
    default: break;
    }
}

#endif // ONE_HAVE_SDL2

// --- settings (settings.h) ------------------------------------------------------------------------------------

static bool apply_binding(const setting *s, const char *value, char *error, size_t error_size)
{
#ifdef ONE_HAVE_SDL2
    return parse_binding_list(value, s->id / INPUT_BUTTONS, s->id % INPUT_BUTTONS, error, error_size);
#else
    (void)s, (void)value; // no SDL2: no keyboard or gamepads, but the settings exist
    if (error_size > 0) {
        error[0] = '\0';
    }
    return true;
#endif
}

static bool apply_hotkey(const setting *s, const char *value, char *error, size_t error_size)
{
#ifdef ONE_HAVE_SDL2
    return parse_hotkey_list(value, s->id, error, error_size);
#else
    (void)s, (void)value;
    if (error_size > 0) {
        error[0] = '\0';
    }
    return true;
#endif
}

static bool apply_input(const setting *s, const char *value, char *error, size_t error_size)
{
    switch (s->id) {
    case 0: {
#ifdef ONE_HAVE_SDL2
        axis_threshold = (int16_t)(32767 * atoi(value) / 100);
#endif
        break;
    }
    case 1: {
        bool on = false;
        settings_parse_bool(value, &on);
#ifdef ONE_HAVE_SDL2
        opposite_directions = on;
#endif
        break;
    }
    case 4: settings_parse_bool(value, &four_score); break;
    case 5: settings_parse_bool(value, &zapper_setting); break;
    case 2:
#ifdef ONE_HAVE_SDL2
        if (*value != '\0' && SDL_GameControllerAddMappingsFromFile(value) < 0) {
            snprintf(error, error_size, "cannot read '%s' (%s)", value, SDL_GetError());
            return false;
        }
#endif
        break;
    case 3:
        for (int i = 0; menu_buttons[i] != NULL; i++) {
            if (strcmp(value, menu_buttons[i]) == 0) {
                menu_button = i;
            }
        }
        break;
    default: break;
    }
    if (error_size > 0) { // (nothing went wrong)
        error[0] = '\0';
    }
    return true;
}

#define JOY(port, button, flags, section)                                                                              \
    {                                                                                                                  \
        "joy" #port "." #button, SETTING_INPUTS, NULL, NULL, section, NULL, 0, 0, NULL, flags, apply_binding, 0        \
    }

static setting controller_table[] = {
    JOY(1, a, 0, "Controller 1"),
    JOY(1, b, 0, NULL),
    JOY(1, select, 0, NULL),
    JOY(1, start, 0, NULL),
    JOY(1, up, 0, NULL),
    JOY(1, down, 0, NULL),
    JOY(1, left, 0, NULL),
    JOY(1, right, 0, NULL),
    JOY(2, a, SETTING_GAP_ABOVE, "Controller 2"),
    JOY(2, b, 0, NULL),
    JOY(2, select, 0, NULL),
    JOY(2, start, 0, NULL),
    JOY(2, up, 0, NULL),
    JOY(2, down, 0, NULL),
    JOY(2, left, 0, NULL),
    JOY(2, right, 0, NULL),
    {"input.axis_threshold", SETTING_INT, "50", "Stick threshold (%)", "Gamepads",
     "how far (percent) a stick must be pushed to press a direction", 1, 99, NULL, SETTING_GAP_ABOVE, apply_input, 0},
    {"input.opposite_directions", SETTING_BOOL, "off", "Left+right, up+down", NULL,
     "on: allow left+right and up+down together (a real pad cannot)", 0, 0, NULL, 0, apply_input, 1},
    {"input.four_score", SETTING_BOOL, "off", "Four Score adapter", NULL,
     "on: the NES Four Score (4 players) is plugged into both ports, as some games (NES Play Action Football) look for it; "
     "players 3 and 4 have no buttons yet",
     0, 0, NULL, 0, apply_input, 4},
    {"input.zapper", SETTING_BOOL, "off", "Zapper (port 2)", NULL,
     "on: a Zapper light gun on port 2 instead of the second controller, aimed with the mouse over the game window "
     "(left button = trigger)",
     0, 0, NULL, 0, apply_input, 5},
    {"input.mappings", SETTING_FILE, "", "More gamepads (file)", NULL,
     "a gamecontrollerdb.txt with more pads for SDL (optional)", 0, 0, NULL, 0, apply_input, 2},
};

#define HOTKEY(name, keys, label, description)                                                                         \
    {                                                                                                                  \
        "hotkey." name, SETTING_KEYS, keys, label, NULL, description, 0, 0, NULL, 0, apply_hotkey, 0                   \
    }

static setting hotkey_table[] = {
    HOTKEY("menu", "key:esc", "Menu", "open the menu (settings, save states, other games...)"),
    HOTKEY("screenshot", "key:shift+f12", "Screenshot",
           "save the picture as a PNG next to the ROM (game-001.png, ...)"),
    HOTKEY("fps", "key:shift+f10", "FPS counter", "show/hide the frames per second in the window title"),
    HOTKEY("fullscreen", "key:alt+enter", "Full screen", "switch between the window and the whole screen"),
    HOTKEY("state_save", "key:f1", "Save state",
           "save the game to the current slot (game.ss1 ... game.ss9, next to the ROM)"),
    HOTKEY("state_load", "key:f2", "Load state", "load the game from the current slot"),
    HOTKEY("slot_previous", "key:shift+f1", "Previous slot", "previous slot (9 after 1)"),
    HOTKEY("slot_next", "key:shift+f2", "Next slot", "next slot (1 after 9)"),
    {"menu.gamepad_button", SETTING_CHOICE, "guide", "Menu on the gamepad", "Gamepad",
     "the gamepad button that opens the menu: guide (the logo button), back+start (both\n"
     "together), select+start (controller 1's Select and Start, whatever they are bound to, together),\n"
     "leftstick, rightstick (pressing the stick) or none",
     0, 0, menu_buttons, SETTING_GAP_ABOVE, apply_input, 3},
};

const setting_group *input_controller_settings(void)
{
    static bool filled = false;
    if (!filled) { // the defaults and labels of joyN.BUTTON come from the tables at the top
        for (int i = 0; i < 2 * INPUT_BUTTONS; i++) {
            controller_table[i].default_value = default_bindings[i / INPUT_BUTTONS][i % INPUT_BUTTONS];
            controller_table[i].label = button_labels[i % INPUT_BUTTONS];
            controller_table[i].id = i;
        }
        for (int i = 0; i < HOTKEYS; i++) {
            hotkey_table[i].id = i;
        }
        filled = true;
    }
    static const setting_group group = {
        "controllers", "Controllers",
        "joyN.BUTTON = the inputs that press NES button BUTTON on controller N, separated by commas (any of\n"
        "them presses it), or 'none'.  An input is:\n"
        "  key:NAME     a key, by its position on a US keyboard: a-z, 0-9, up, down, left, right, space,\n"
        "               enter, tab, backspace, esc, lshift, rshift, lctrl, rctrl, lalt, ralt, comma, period,\n"
        "               slash, backslash, semicolon, apostrophe, leftbracket, rightbracket, minus, equals,\n"
        "               kp0-kp9 (keypad), or any SDL key name with '_' for spaces (key:right_shift)\n"
        "  padN:NAME    a button of gamepad N (pad1 = the first one connected).  The names follow the Xbox\n"
        "               layout: a (bottom) b (right) x (left) y (top), back, start, guide, leftshoulder,\n"
        "               rightshoulder, leftstick, rightstick, dpup, dpdown, dpleft, dpright; a stick or\n"
        "               trigger pushed one way: leftx- leftx+ lefty- lefty+ rightx- rightx+ righty- righty+\n"
        "               lefttrigger+ righttrigger+\n"
        "  padN:buttonK padN:axisK+ padN:axisK- padN:hatK_up (_down _left _right)\n"
        "               raw inputs, for a pad SDL does not recognise.\n"
        "`one --list-gamepads` lists the pads and prints the names of their buttons as you press them.",
        controller_table, (int)(sizeof(controller_table) / sizeof(controller_table[0]))};
    return &group;
}

const setting_group *input_hotkey_settings(void)
{
    input_controller_settings(); // (fills in the tables)
    static const setting_group group = {
        "hotkeys (in the game window)", "Hotkeys",
        "hotkey.NAME = keys that do it, separated by commas, or 'none': key:NAME as above, with\n"
        "shift+ ctrl+ alt+ in front for keys held with it (key:shift+f12).",
        hotkey_table, (int)(sizeof(hotkey_table) / sizeof(hotkey_table[0]))};
    return &group;
}

// --- the menu (frontend.h) -------------------------------------------------------------------------------------

#ifdef ONE_HAVE_SDL2
static int pad_slot(SDL_JoystickID id)
{
    for (int i = 0; i < MAX_PADS; i++) {
        if (pads[i].joystick != NULL && pads[i].id == id) {
            return i;
        }
    }
    return -1;
}

// key:NAME for a scancode: the friendly names of key_from_name(), else SDL's name in lower case with '_'
static void key_name(SDL_Scancode sc, char *name, size_t size)
{
    static const char *const friendly[] = {
        "enter", "esc",    "lshift", "rshift",    "lctrl",     "rctrl",      "lalt",        "ralt",
        "comma", "period", "slash",  "backslash", "semicolon", "apostrophe", "leftbracket", "rightbracket",
        "minus", "equals", "grave",  "kpenter",   "kpplus",    "kpminus"};
    for (size_t i = 0; i < sizeof(friendly) / sizeof(friendly[0]); i++) {
        if (key_from_name(friendly[i]) == sc) {
            snprintf(name, size, "%s", friendly[i]);
            return;
        }
    }
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_0) {
        snprintf(name, size, "kp%d", sc == SDL_SCANCODE_KP_0 ? 0 : (int)(sc - SDL_SCANCODE_KP_1) + 1);
        return;
    }
    snprintf(name, size, "%s", SDL_GetScancodeName(sc));
    for (char *c = name; *c != '\0'; c++) {
        *c = (*c == ' ') ? '_' : (char)tolower((unsigned char)*c);
    }
}

static bool is_modifier(SDL_Scancode sc)
{
    return sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT || sc == SDL_SCANCODE_LCTRL ||
           sc == SDL_SCANCODE_RCTRL || sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT || sc == SDL_SCANCODE_LGUI ||
           sc == SDL_SCANCODE_RGUI;
}

// Does SDL's mapping of a known pad give raw button number `button` a name (a, b, back, ...)?
static bool raw_button_is_named(SDL_GameController *controller, int button)
{
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
        SDL_GameControllerButtonBind bind = SDL_GameControllerGetBindForButton(controller, (SDL_GameControllerButton)i);
        if (bind.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON && bind.value.button == button) {
            return true;
        }
    }
    return false;
}
#endif

bool fe_input_name(const void *sdl_event, bool keys_only, char *name, size_t name_size)
{
#ifdef ONE_HAVE_SDL2
    const SDL_Event *event = sdl_event;
    char text[64];
    int slot = -1;
    switch (event->type) {
    case SDL_KEYDOWN:
        if (event->key.repeat != 0 || (keys_only && is_modifier(event->key.keysym.scancode))) {
            return false;
        }
        key_name(event->key.keysym.scancode, text, sizeof(text));
        if (keys_only) {
            Uint16 mod = event->key.keysym.mod;
            snprintf(name, name_size, "key:%s%s%s%s", (mod & KMOD_SHIFT) ? "shift+" : "",
                     (mod & KMOD_CTRL) ? "ctrl+" : "", (mod & KMOD_ALT) ? "alt+" : "", text);
        } else {
            snprintf(name, name_size, "key:%s", text);
        }
        return true;
    case SDL_CONTROLLERBUTTONDOWN:
        slot = pad_slot(event->cbutton.which);
        if (keys_only || slot < 0) {
            return false;
        }
        snprintf(name, name_size, "pad%d:%s", slot + 1,
                 SDL_GameControllerGetStringForButton((SDL_GameControllerButton)event->cbutton.button));
        return true;
    case SDL_CONTROLLERAXISMOTION:
        slot = pad_slot(event->caxis.which);
        if (keys_only || slot < 0 || (event->caxis.value <= axis_threshold && event->caxis.value >= -axis_threshold)) {
            return false;
        }
        snprintf(name, name_size, "pad%d:%s%c", slot + 1,
                 SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)event->caxis.axis),
                 event->caxis.value > 0 ? '+' : '-');
        return true;
    // raw inputs: for pads SDL does not know, and for the buttons its mapping for a known pad leaves out (a pad whose
    // Select SDL has no name for sends only the raw event; for the other buttons the named event above comes too)
    case SDL_JOYBUTTONDOWN:
        slot = pad_slot(event->jbutton.which);
        if (keys_only || slot < 0 ||
            (pads[slot].controller != NULL && raw_button_is_named(pads[slot].controller, event->jbutton.button))) {
            return false;
        }
        snprintf(name, name_size, "pad%d:button%d", slot + 1, event->jbutton.button);
        return true;
    case SDL_JOYAXISMOTION:
        slot = pad_slot(event->jaxis.which);
        if (keys_only || slot < 0 || pads[slot].controller != NULL ||
            (event->jaxis.value <= axis_threshold && event->jaxis.value >= -axis_threshold)) {
            return false;
        }
        snprintf(name, name_size, "pad%d:axis%d%c", slot + 1, event->jaxis.axis, event->jaxis.value > 0 ? '+' : '-');
        return true;
    case SDL_JOYHATMOTION: {
        slot = pad_slot(event->jhat.which);
        if (keys_only || slot < 0 || pads[slot].controller != NULL || event->jhat.value == SDL_HAT_CENTERED) {
            return false;
        }
        const char *dir = (event->jhat.value & SDL_HAT_UP)     ? "up"
                          : (event->jhat.value & SDL_HAT_DOWN) ? "down"
                          : (event->jhat.value & SDL_HAT_LEFT) ? "left"
                                                               : "right";
        snprintf(name, name_size, "pad%d:hat%d_%s", slot + 1, event->jhat.hat, dir);
        return true;
    }
    default: return false;
    }
#else
    (void)sdl_event, (void)keys_only;
    if (name_size > 0) {
        name[0] = '\0';
    }
    return false;
#endif
}

bool fe_is_menu_button(const void *sdl_event)
{
#ifdef ONE_HAVE_SDL2
    const SDL_Event *event = sdl_event;
    if (event->type == SDL_KEYDOWN) { // hotkey.menu
        int modifiers = ((event->key.keysym.mod & KMOD_SHIFT) ? HOTKEY_SHIFT : 0) |
                        ((event->key.keysym.mod & KMOD_CTRL) ? HOTKEY_CTRL : 0) |
                        ((event->key.keysym.mod & KMOD_ALT) ? HOTKEY_ALT : 0);
        for (int i = 0; i < hotkey_count[HOTKEY_MENU]; i++) {
            if (hotkeys[HOTKEY_MENU][i].key == event->key.keysym.scancode &&
                hotkeys[HOTKEY_MENU][i].modifiers == modifiers && event->key.repeat == 0) {
                return true;
            }
        }
        return false;
    }
    if (menu_button == MENU_SELECT_START) { // controller 1's Select and Start, from any of their inputs
        if (event->type != SDL_CONTROLLERBUTTONDOWN && event->type != SDL_JOYBUTTONDOWN &&
            event->type != SDL_JOYHATMOTION && event->type != SDL_JOYAXISMOTION) {
            return false;
        }
        const uint8_t both = (1u << BTN_SELECT) | (1u << BTN_START);
        bool held = (read_port(0) & both) == both;
        bool pressed = held && !select_start_held;
        select_start_held = held;
        return pressed;
    }
    if (event->type != SDL_CONTROLLERBUTTONDOWN) {
        return false;
    }
    int slot = pad_slot(event->cbutton.which);
    if (slot < 0 || pads[slot].controller == NULL) {
        return false;
    }
    SDL_GameController *pad = pads[slot].controller;
    Uint8 button = event->cbutton.button;
    switch (menu_button) {
    case MENU_GUIDE: return button == SDL_CONTROLLER_BUTTON_GUIDE;
    case MENU_BACK_START:
        return (button == SDL_CONTROLLER_BUTTON_BACK &&
                SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) ||
               (button == SDL_CONTROLLER_BUTTON_START && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK));
    case MENU_LEFTSTICK: return button == SDL_CONTROLLER_BUTTON_LEFTSTICK;
    case MENU_RIGHTSTICK: return button == SDL_CONTROLLER_BUTTON_RIGHTSTICK;
    default: return false;
    }
#else
    (void)sdl_event;
    return false;
#endif
}

bool fe_window_event(const void *sdl_event)
{
#ifdef ONE_HAVE_SDL2
    const SDL_Event *event = sdl_event;
    switch (event->type) {
    case SDL_QUIT: cpu_request_stop(); return true;
    case SDL_JOYDEVICEADDED:
        if (gamepads_enabled) {
            pad_added(event->jdevice.which);
        }
        break;
    case SDL_JOYDEVICEREMOVED:
        if (gamepads_enabled) {
            pad_removed(event->jdevice.which);
        }
        break;
    default: break;
    }
#else
    (void)sdl_event;
#endif
    return false;
}

// The menu was asked for during this frame: open it now, then do what it says
static void run_menu(void)
{
    menu_requested = false;
    switch (fe_run_menu(true)) {
    case FE_QUIT: cpu_request_stop(); break;
    case FE_RESET: cpu_request_soft_reset(); break;
    case FE_POWER_CYCLE:
        fe_set_next_rom(fe_rom_path()); // one.c starts it again
        cpu_request_stop();
        break;
    case FE_OPEN_ROM: cpu_request_stop(); break; // one.c starts the game the menu chose
    case FE_SAVE_STATE: savestate_save_slot(); break;
    case FE_LOAD_STATE: savestate_load_slot(); break;
    default: break;
    }
}

// --- polling ------------------------------------------------------------------------------------------------

void input_init(void)
{
#ifdef ONE_HAVE_SDL2
    if (video_is_headless() || gamepads_enabled) {
        return; // no window: no keyboard or gamepad input (tests, traces)
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"); // pads work while the debugger has the focus
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        LOG_WARN(INPUT, "no gamepad support (%s)", SDL_GetError());
        return;
    }
    gamepads_enabled = true; // the pads already connected arrive as SDL_JOYDEVICEADDED events
#endif
}

void input_shutdown(void)
{
#ifdef ONE_HAVE_SDL2
    if (!gamepads_enabled) {
        return;
    }
    for (int i = 0; i < MAX_PADS; i++) {
        if (pads[i].joystick != NULL) {
            pad_removed(pads[i].id);
        }
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    gamepads_enabled = false;
#endif
}

// Called once per frame by the CPU: handle window events (no-op without SDL2 or before the window exists).
void input_poll(void)
{
#ifdef ONE_HAVE_SDL2
    if (SDL_WasInit(SDL_INIT_EVENTS)) {
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            if ((event_hook == NULL || !event_hook(&event, event_hook_context)) && !script_window_event(&event)) {
                input_handle_event(&event);
            }
        }
        quit_from_close = false; // (the SDL_QUIT that follows a close comes in the same batch)
    }
#endif
    if (menu_requested) {
        run_menu();
    }
    video_fps_tick();
    video_overlay_tick();
}

void input_wait(void)
{
#ifdef ONE_HAVE_SDL2
    if (SDL_WasInit(SDL_INIT_EVENTS)) {
        SDL_Event event;
        if (SDL_WaitEvent(&event) && (event_hook == NULL || !event_hook(&event, event_hook_context)) &&
            !script_window_event(&event)) {
            input_handle_event(&event);
        }
    }
#endif
}

void input_set_event_hook(input_event_hook hook, void *context)
{
    event_hook = hook;
    event_hook_context = context;
}

// --- what the game reads, and what scripts change -----------------------------------------------------------

static int16_t script_override[INPUT_PORTS] = {-1, -1}; // emu.setInput(): buttons for this frame (-1: none)
static uint8_t held_buttons[INPUT_PORTS];               // input_hold() (the debugger's 'press'):
static uint64_t held_until[INPUT_PORTS];                // ... held until this CPU cycle
static bool polled_since_frame = false;                 // the game polled the controllers in this frame
static uint32_t lag_count = 0;                          // frames in which it did not
static bool last_frame_lagged = false;                  // ... and whether the last one was

void input_set_override(int port, uint8_t buttons)
{
    if (port >= 0 && port < INPUT_PORTS) {
        script_override[port] = buttons;
    }
}

void input_clear_overrides(void)
{
    script_zapper_set = false;
    for (int port = 0; port < INPUT_PORTS; port++) {
        script_override[port] = -1;
    }
}

void input_note_poll(void)
{
    polled_since_frame = true;
}

void input_frame_done(void)
{
    last_frame_lagged = !polled_since_frame;
    polled_since_frame = false;
    if (last_frame_lagged) {
        lag_count++;
    }
}

uint32_t input_lag_count(void)
{
    return lag_count;
}

bool input_last_frame_lagged(void)
{
    return last_frame_lagged;
}

void input_reset_lag_count(void)
{
    lag_count = 0;
    last_frame_lagged = false;
    polled_since_frame = false;
}

// The lag counter is part of a save state: loading one gives back the count it was saved with
void input_state(state_io *io)
{
    STATE(io, polled_since_frame);
    STATE(io, lag_count);
    STATE(io, last_frame_lagged);
}

// The buttons of a port as the game would read them now: keyboard / gamepad, or what a script set
uint8_t input_buttons(int port)
{
    if (port < 0 || port >= INPUT_PORTS) {
        return 0;
    }
    if (script_override[port] >= 0) {
        return (uint8_t)script_override[port];
    }
    uint8_t state = 0;
#ifdef ONE_HAVE_SDL2
    state = read_port(port);
#endif
    if (port == 0 && scripted_start && ((cpu_get_cycle() - scripted_start_cycle) < SCRIPTED_PRESS_CYCLES)) {
        state |= (1 << BTN_START);
    }
    if (script_count > 0) {
        state |= scripted_buttons(port);
    }
    if (cpu_get_cycle() < held_until[port]) {
        state |= held_buttons[port];
    }
    return state;
}

void input_hold(int port, uint8_t buttons, uint64_t until_cycle)
{
    if (port >= 0 && port < INPUT_PORTS) {
        held_buttons[port] = buttons;
        held_until[port] = until_cycle;
    }
}

// http://wiki.nesdev.com/w/index.php/Standard_controller (the strobe latches the buttons)
uint8_t input_joy1_read(void)
{
    polled_since_frame = true;
    return input_buttons(0);
}

bool input_four_score(void)
{
    return four_score;
}

uint8_t input_joy2_read(void)
{
    return input_buttons(1);
}

bool input_key_pressed(const char *name)
{
#ifdef ONE_HAVE_SDL2
    if (!SDL_WasInit(SDL_INIT_EVENTS) || name == NULL) {
        return false;
    }
    SDL_Scancode code = SDL_GetScancodeFromName(name);
    if (code == SDL_SCANCODE_UNKNOWN) {
        return false;
    }
    return SDL_GetKeyboardState(NULL)[code] != 0;
#else
    (void)name;
    return false;
#endif
}

// --- one --list-gamepads ------------------------------------------------------------------------------------

int input_list_gamepads(void)
{
#ifdef ONE_HAVE_SDL2
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"); // no window has the focus here
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "cannot start SDL's gamepad support: %s\n", SDL_GetError());
        return 1;
    }
    int count = SDL_NumJoysticks();
    printf("%d gamepad%s connected.\n", count, count == 1 ? "" : "s");
    for (int i = 0; i < count && i < MAX_PADS; i++) {
        char guid[64];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));
        printf("  pad%d: %s\n        %s; GUID %s\n", i + 1, SDL_JoystickNameForIndex(i),
               SDL_IsGameController(i) ? "named buttons work (SDL knows this pad)"
                                       : "SDL does not know this pad: use raw names",
               guid);
    }
    printf("Press buttons and move the sticks to see their names (Ctrl+C to stop).\n");
    fflush(stdout);
    gamepads_enabled = true;
    int8_t axis_state[MAX_PADS][16] = {{0}};
    int8_t named_axis_state[MAX_PADS][SDL_CONTROLLER_AXIS_MAX] = {{0}};
    bool running = true;
    while (running) {
        SDL_Event event;
        if (!SDL_WaitEvent(&event)) {
            break;
        }
        int slot = -1;
        SDL_JoystickID id = -1;
        switch (event.type) {
        case SDL_JOYBUTTONDOWN: id = event.jbutton.which; break;
        case SDL_JOYAXISMOTION: id = event.jaxis.which; break;
        case SDL_JOYHATMOTION: id = event.jhat.which; break;
        case SDL_CONTROLLERBUTTONDOWN: id = event.cbutton.which; break;
        case SDL_CONTROLLERAXISMOTION: id = event.caxis.which; break;
        default: break;
        }
        for (int i = 0; i < MAX_PADS; i++) {
            if (pads[i].joystick != NULL && pads[i].id == id) {
                slot = i;
            }
        }
        switch (event.type) {
        case SDL_QUIT: running = false; break;
        case SDL_JOYDEVICEADDED: pad_added(event.jdevice.which); break;
        case SDL_JOYDEVICEREMOVED: pad_removed(event.jdevice.which); break;
        case SDL_CONTROLLERBUTTONDOWN:
            if (slot >= 0) {
                printf("  pad%d:%s\n", slot + 1,
                       SDL_GameControllerGetStringForButton((SDL_GameControllerButton)event.cbutton.button));
            }
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (slot >= 0 && event.caxis.axis < SDL_CONTROLLER_AXIS_MAX) {
                int8_t now = event.caxis.value > axis_threshold ? 1 : event.caxis.value < -axis_threshold ? -1 : 0;
                if (now != 0 && now != named_axis_state[slot][event.caxis.axis]) {
                    printf("  pad%d:%s%c\n", slot + 1,
                           SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)event.caxis.axis),
                           now > 0 ? '+' : '-');
                }
                named_axis_state[slot][event.caxis.axis] = now;
            }
            break;
        case SDL_JOYBUTTONDOWN:
            if (slot >= 0) {
                printf("  pad%d:button%d   (raw)\n", slot + 1, event.jbutton.button);
            }
            break;
        case SDL_JOYAXISMOTION:
            if (slot >= 0 && event.jaxis.axis < 16) {
                int8_t now = event.jaxis.value > axis_threshold ? 1 : event.jaxis.value < -axis_threshold ? -1 : 0;
                if (now != 0 && now != axis_state[slot][event.jaxis.axis]) {
                    printf("  pad%d:axis%d%c   (raw)\n", slot + 1, event.jaxis.axis, now > 0 ? '+' : '-');
                }
                axis_state[slot][event.jaxis.axis] = now;
            }
            break;
        case SDL_JOYHATMOTION:
            if (slot >= 0 && event.jhat.value != SDL_HAT_CENTERED) {
                const char *dir = (event.jhat.value & SDL_HAT_UP)     ? "up"
                                  : (event.jhat.value & SDL_HAT_DOWN) ? "down"
                                  : (event.jhat.value & SDL_HAT_LEFT) ? "left"
                                                                      : "right";
                printf("  pad%d:hat%d_%s   (raw)\n", slot + 1, event.jhat.hat, dir);
            }
            break;
        default: break;
        }
        fflush(stdout);
    }
    input_shutdown();
    SDL_Quit();
    return 0;
#else
    fprintf(stderr, "this build has no SDL2 support: no gamepads\n");
    return 1;
#endif
}
