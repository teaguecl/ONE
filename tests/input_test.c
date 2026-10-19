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
// Gamepad input test (ctest input_gamepads): SDL virtual gamepads stand in for real ones, so the controller
// bindings of one.cfg (named buttons, sticks, raw buttons and hats, two ports, hot-plugging) can be checked
// without hardware. Also the game window's hotkeys (screenshot, FPS counter), with synthetic key events.  Needs SDL 2.24+ and a video driver; ctest runs it with SDL_VIDEODRIVER=dummy.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define SDL_MAIN_HANDLED // a plain main(), also on Windows
#include <SDL.h>
#include "input.h"
#include "settings.h"
#include "video.h"

enum { A = 1, B = 2, SELECT = 4, START = 8, UP = 16, DOWN = 32, LEFT = 64, RIGHT = 128 };

static int failures = 0;

static void expect(const char *what, uint8_t got, uint8_t want)
{
    if (got != want) {
        printf("FAIL %s: got %02X, want %02X\n", what, got, want);
        failures++;
    } else {
        printf("ok   %s\n", what);
    }
}

// The virtual pads' USB ids (pid.codes test ids). Only these are let in, so a real gamepad plugged into the machine
// running the tests cannot take pad1 (SDL numbers the pads in the order they connect).
#define TEST_PAD_VID  0x1209
#define TEST_PAD_PID  0x0001
#define TEST_PAD_ONLY "0x1209/0x0001"

// a virtual gamepad that SDL treats like a known controller (buttons/axes in SDL_GameController order),
// plus one hat for the raw hat bindings
static SDL_Joystick *attach_pad(void)
{
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.nhats = 1;
    desc.vendor_id = TEST_PAD_VID;
    desc.product_id = TEST_PAD_PID;
    int index = SDL_JoystickAttachVirtualEx(&desc);
    if (index < 0) {
        printf("cannot attach a virtual gamepad: %s\n", SDL_GetError());
        exit(2);
    }
    SDL_Joystick *joystick = SDL_JoystickOpen(index);
    input_poll(); // the emulator opens the new pad (SDL_JOYDEVICEADDED)
    return joystick;
}

static void set_button(SDL_Joystick *pad, SDL_GameControllerButton button, int down)
{
    SDL_JoystickSetVirtualButton(pad, button, (Uint8)down);
    SDL_JoystickUpdate();
    input_poll();
}

static void set_axis(SDL_Joystick *pad, SDL_GameControllerAxis axis, int value)
{
    SDL_JoystickSetVirtualAxis(pad, axis, (Sint16)value);
    SDL_JoystickUpdate();
    input_poll();
}

static void press_key(SDL_Scancode key, Uint16 mod)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_KEYDOWN;
    event.key.keysym.scancode = key;
    event.key.keysym.sym = SDL_GetKeyFromScancode(key);
    event.key.keysym.mod = mod;
    event.key.state = SDL_PRESSED;
    SDL_PushEvent(&event);
    input_poll();
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}

static void expect_true(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

static void set_hat(SDL_Joystick *pad, Uint8 value)
{
    SDL_JoystickSetVirtualHat(pad, 0, value);
    SDL_JoystickUpdate();
    input_poll();
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, TEST_PAD_ONLY); // (before SDL starts)
    // the defaults, plus raw bindings: pad1 button 2 (x) = select as well, pad2's hat = up/down
    const char *path = "input_test.cfg";
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return 2;
    }
    fprintf(f, "joy1.select = pad1:back, pad1:button2\n"
               "joy2.up = pad2:hat0_up\n"
               "joy2.down = pad2:hat0_down\n"
               "input.axis_threshold = 50\n"
               "hotkey.fps = key:ctrl+alt+f\n");
    fclose(f);
    if (settings_load(path) != 0) {
        printf("FAIL: the settings file has errors\n");
        return 1;
    }
    remove(path);

    if (!video_init(false)) {
        printf("no video driver (set SDL_VIDEODRIVER=dummy)\n");
        return 2;
    }
    input_init();
    expect("nothing connected", input_joy1_read(), 0);

    SDL_Joystick *pad1 = attach_pad();
    set_button(pad1, SDL_CONTROLLER_BUTTON_B, 1);
    expect("pad1 b (right face button) = NES A", input_joy1_read(), A);
    set_button(pad1, SDL_CONTROLLER_BUTTON_B, 0);
    set_button(pad1, SDL_CONTROLLER_BUTTON_A, 1);
    set_button(pad1, SDL_CONTROLLER_BUTTON_START, 1);
    expect("pad1 a + start = NES B + Start", input_joy1_read(), B | START);
    set_button(pad1, SDL_CONTROLLER_BUTTON_A, 0);
    set_button(pad1, SDL_CONTROLLER_BUTTON_START, 0);
    set_button(pad1, SDL_CONTROLLER_BUTTON_X, 1);
    expect("pad1 raw button2 (x) = Select", input_joy1_read(), SELECT);
    set_button(pad1, SDL_CONTROLLER_BUTTON_X, 0);
    set_button(pad1, SDL_CONTROLLER_BUTTON_DPAD_UP, 1);
    expect("pad1 d-pad up", input_joy1_read(), UP);
    set_button(pad1, SDL_CONTROLLER_BUTTON_DPAD_UP, 0);
    set_axis(pad1, SDL_CONTROLLER_AXIS_LEFTX, -30000);
    expect("pad1 left stick pushed left", input_joy1_read(), LEFT);
    set_axis(pad1, SDL_CONTROLLER_AXIS_LEFTX, -10000);
    expect("pad1 left stick barely moved (under 50%)", input_joy1_read(), 0);
    set_axis(pad1, SDL_CONTROLLER_AXIS_LEFTX, 30000);
    set_button(pad1, SDL_CONTROLLER_BUTTON_DPAD_LEFT, 1);
    expect("left + right together are dropped", input_joy1_read(), 0);
    set_axis(pad1, SDL_CONTROLLER_AXIS_LEFTX, 0);
    set_button(pad1, SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0);
    expect("pad1 released", input_joy1_read(), 0);
    expect("port 2 empty with one pad", input_joy2_read(), 0);

    SDL_Joystick *pad2 = attach_pad();
    set_button(pad2, SDL_CONTROLLER_BUTTON_START, 1);
    set_hat(pad2, SDL_HAT_DOWN);
    expect("pad2 start + raw hat down = port 2", input_joy2_read(), START | DOWN);
    expect("pad2 does not press port 1", input_joy1_read(), 0);
    set_button(pad2, SDL_CONTROLLER_BUTTON_START, 0);
    set_hat(pad2, SDL_HAT_CENTERED);

    set_button(pad1, SDL_CONTROLLER_BUTTON_B, 1);
    SDL_JoystickClose(pad1);
    SDL_JoystickDetachVirtual(0); // unplugged while the button is held
    input_poll();
    expect("pad1 unplugged", input_joy1_read(), 0);
    set_button(pad2, SDL_CONTROLLER_BUTTON_B, 1);
    expect("pad2 still on port 2", input_joy2_read(), A);

    // hotkeys: Shift+F12 (default) saves a screenshot named after the ROM; F12 alone is not a hotkey;
    // the FPS counter was moved to Ctrl+Alt+F by the settings above, so Shift+F10 no longer does it
    remove("input_test-001.png");
    remove("input_test-002.png");
    video_set_rom_path("input_test.nes");
    press_key(SDL_SCANCODE_F12, KMOD_LSHIFT);
    expect_true("Shift+F12 saves input_test-001.png", file_exists("input_test-001.png"));
    press_key(SDL_SCANCODE_F12, KMOD_NONE);
    press_key(SDL_SCANCODE_F12, KMOD_LSHIFT | KMOD_LCTRL);
    expect_true("F12 and Ctrl+Shift+F12 are not the screenshot key", !file_exists("input_test-002.png"));
    press_key(SDL_SCANCODE_F10, KMOD_RSHIFT);
    expect_true("Shift+F10 was rebound away", !video_fps_shown());
    press_key(SDL_SCANCODE_F, KMOD_LCTRL | KMOD_LALT | KMOD_NUM);
    expect_true("Ctrl+Alt+F (Num Lock on) shows the FPS counter", video_fps_shown());
    press_key(SDL_SCANCODE_F, KMOD_RCTRL | KMOD_RALT);
    expect_true("again: hides it", !video_fps_shown());
    remove("input_test-001.png");

    input_shutdown();
    video_shutdown();
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
