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
// The menu, driven like a user would (ctest gui_menu): SDL's dummy video driver, key presses and a virtual
// gamepad injected frame by frame (gui_set_frame_hook()), and checks of what the menu changed. With
// GUI_TEST_SHOTS=DIR it also writes a PNG of the window at each "shot" step (to look at the menu).
//
// (A test, not part of the menu: besides frontend.h it uses the emulator's video.h/input.h/png.h to set up a window.)
#define SDL_MAIN_HANDLED // (our own main(): on Windows SDL.h would rename it to SDL_main and ask for SDL2main)
#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "frontend.h"
#include "gui.h"
#include "imgui.h"
#include "imgui_internal.h" // (the keyboard focus, to see where it has been)
#include "settings.h"

extern "C" {
#include "cpu.h"
#include "emu.h"
#include "input.h"
#include "png.h"
#include "savestate.h"
#include "scheduler.h"
#include "video.h"
extern uint64_t glb_clock_stop; // cpu.c (-k)
}

namespace {

int failures = 0;
int checks = 0;

void check(const std::string &what, bool ok)
{
    checks++;
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) {
        failures++;
    }
}

bool settingIs(const char *key, const char *value)
{
    const char *now = settings_get(key);
    if (now == nullptr || std::strcmp(now, value) != 0) {
        std::printf("      %s = '%s' (expected '%s')\n", key, now ? now : "(none)", value);
        return false;
    }
    return true;
}

// --- the script: what to do at which frame of the menu ------------------------------------------------------------

struct Step {
    int frame;
    std::function<void()> action;
};
std::vector<Step> script;
int scriptFrame = 0; // the frame the next step() is scheduled for
int lastFrame = 0;   // the menu's current frame (in the hook)

void at(int framesLater, std::function<void()> action)
{
    scriptFrame += framesLater;
    script.push_back({scriptFrame, std::move(action)});
}

// The keyboard focus items seen while tracking (one per frame where it changed)
bool trackFocus = false;
std::vector<ImGuiID> focusSeen;

void hook(int frame)
{
    lastFrame = frame;
    if (trackFocus && ImGui::GetCurrentContext() != nullptr) {
        ImGuiID id = ImGui::GetCurrentContext()->NavId;
        if (focusSeen.empty() || focusSeen.back() != id) {
            focusSeen.push_back(id);
        }
    }
    for (const Step &step : script) {
        if (step.frame == frame) {
            step.action();
        }
    }
    if (frame == scriptFrame + 120) { // the script is over but the menu is still open: close the window
        check("the menu closed at the end of the script", false);
        SDL_Event quit{};
        quit.type = SDL_QUIT;
        SDL_PushEvent(&quit);
    }
}

void pushKey(SDL_Keycode sym, Uint16 mod = KMOD_NONE)
{
    SDL_Event e{};
    e.type = SDL_KEYDOWN;
    e.key.state = SDL_PRESSED;
    e.key.keysym.sym = sym;
    e.key.keysym.scancode = SDL_GetScancodeFromKey(sym);
    e.key.keysym.mod = mod;
    e.key.windowID = SDL_GetWindowID(static_cast<SDL_Window *>(fe_sdl_window()));
    SDL_PushEvent(&e);
    e.type = SDL_KEYUP;
    e.key.state = SDL_RELEASED;
    SDL_PushEvent(&e);
}

// A key press, 4 frames after the previous step (Dear ImGui sees one input event per frame)
void key(SDL_Keycode sym, Uint16 mod = KMOD_NONE)
{
    at(4, [sym, mod] { pushKey(sym, mod); });
}

void keys(SDL_Keycode sym, int times)
{
    for (int i = 0; i < times; i++) {
        key(sym);
    }
}

void expect(const std::string &what, const std::function<bool()> &test)
{
    at(4, [what, test] { check(what, test()); });
}

const char *shotDir = nullptr;
void shot(const std::string &name)
{
    at(2, [name] {
        if (shotDir == nullptr) {
            return;
        }
        auto *renderer = static_cast<SDL_Renderer *>(fe_sdl_renderer());
        int w = 0;
        int h = 0;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        std::vector<unsigned char> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
        SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGB24, rgb.data(), w * 3);
        std::string path = std::string(shotDir) + "/" + name + ".png";
        png_write_rgb(path.c_str(), rgb.data(), w, h);
        std::printf("      (picture: %s)\n", path.c_str());
    });
}

fe_action runMenu(bool gameRunning)
{
    fe_action action = fe_run_menu(gameRunning);
    script.clear();
    scriptFrame = 0;
    return action;
}

// --- a virtual gamepad (as in tests/input_test.c) -----------------------------------------------------------------

SDL_Joystick *pad = nullptr;

// The virtual pad's USB ids (pid.codes test ids). Only these are let in, so a real gamepad plugged into the machine
// running the tests cannot take pad1 (SDL numbers the pads in the order they connect).
constexpr Uint16 kTestPadVid = 0x1209, kTestPadPid = 0x0001;
constexpr const char *kTestPadOnly = "0x1209/0x0001";

void attachPad()
{
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.vendor_id = kTestPadVid;
    desc.product_id = kTestPadPid;
    int index = SDL_JoystickAttachVirtualEx(&desc);
    if (index < 0) {
        std::printf("cannot attach a virtual gamepad: %s\n", SDL_GetError());
        std::exit(2);
    }
    pad = SDL_JoystickOpen(index);
    input_poll(); // the emulator opens it (pad1)
}

// A gamepad button pressed for 3 frames, then let go
void button(SDL_GameControllerButton b)
{
    at(4, [b] {
        SDL_JoystickSetVirtualButton(pad, b, 1);
        SDL_JoystickUpdate();
    });
    at(3, [b] {
        SDL_JoystickSetVirtualButton(pad, b, 0);
        SDL_JoystickUpdate();
    });
}

// PATH has the line "KEY = VALUE" (any spacing, maybe a comment after it)
bool fileHasSetting(const char *path, const std::string &key, const std::string &value)
{
    FILE *f = std::fopen(path, "r");
    if (f == nullptr) {
        return false;
    }
    char line[1024];
    bool found = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string l = line;
        size_t eq = l.find('=');
        if (eq == std::string::npos || l.compare(0, key.size(), key) != 0 ||
            l.find_first_not_of(' ', key.size()) != eq) {
            continue;
        }
        std::string v = l.substr(eq + 1);
        size_t hash = v.find(" #");
        v = v.substr(0, hash);
        v.erase(0, v.find_first_not_of(' '));
        v.erase(v.find_last_not_of(" \n") + 1);
        found = v == value;
    }
    std::fclose(f);
    return found;
}

bool fileHas(const char *path, const char *text)
{
    FILE *f = std::fopen(path, "r");
    if (f == nullptr) {
        return false;
    }
    std::string all;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        all.append(buf, n);
    }
    std::fclose(f);
    return all.find(text) != std::string::npos;
}

// The sidebar: Start/Game, Lua scripts, Controllers, Hotkeys, Menu, Video, Sound, Logging, Cartridge, Game Genie,
// (Resume), Quit. Esc (no game) goes back
// to the sidebar's current page; from there DOWN/UP moves, Space chooses.
void page(int fromIndex, int toIndex)
{
    key(SDLK_ESCAPE);
    keys(toIndex > fromIndex ? SDLK_DOWN : SDLK_UP, toIndex > fromIndex ? toIndex - fromIndex : fromIndex - toIndex);
    key(SDLK_SPACE);
}

void copyFile(const char *from, const char *to)
{
    FILE *in = std::fopen(from, "rb");
    FILE *out = std::fopen(to, "wb");
    if (in != nullptr && out != nullptr) {
        char buf[65536];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
            std::fwrite(buf, 1, n, out);
        }
    }
    if (in != nullptr) {
        std::fclose(in);
    }
    if (out != nullptr) {
        std::fclose(out);
    }
}

const char *romPath = nullptr; // argv[1]: a ROM for the in-game part (nestest)

} // namespace

int main(int argc, char **argv)
{
    romPath = argc > 1 ? argv[1] : nullptr;
    std::setvbuf(stdout, nullptr, _IOLBF, 4096); // (lines appear as they happen; MSVC refuses a size below 2)
    shotDir = std::getenv("GUI_TEST_SHOTS");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, kTestPadOnly); // (before SDL starts)
    // the recently opened games go to a directory of the test's own (never the user's list)
#ifdef _WIN32
    _putenv_s("APPDATA", "gui_test_state");
#else
    setenv("XDG_STATE_HOME", "gui_test_state", 1);
    // (settings_load() falls back from $XDG_CONFIG_HOME to ~/.config: without this the test loaded, changed and
    // saved the user's own one.cfg)
    setenv("HOME", "gui_test_state", 1);
#endif
    fe_recent_clear(FE_RECENT_GAMES);
    fe_recent_clear(FE_RECENT_SCRIPTS);
    std::remove("one.cfg");
    char userFile[512];
    settings_user_path(userFile, sizeof(userFile));
    std::remove(userFile); // (left by the last run: start from the defaults)
    if (settings_load(nullptr) != 0) {
        std::printf("FAIL: the settings did not load\n");
        return 1;
    }
    if (!video_init(false)) {
        std::printf("no video driver (set SDL_VIDEODRIVER=dummy)\n");
        return 2;
    }
    input_init();
    attachPad();
    gui_install();
    gui_set_frame_hook(hook);
    check("the menu is there", fe_menu_available());
    std::string saveTo = settings_path();

    // --- 1. No game: the pages, with the keyboard ---------------------------------------------------------------
    shot("start");
    page(0, 2);
    shot("controllers");
    page(2, 3);
    shot("hotkeys");
    page(3, 4);
    shot("menu");
    key(SDLK_SPACE); // menu.hint: the first setting of the page
    expect("Menu: Space switches the start-up hint off", [] { return settingIs("menu.hint", "off"); });
    key(SDLK_SPACE);
    page(4, 5);
    shot("video");
    page(5, 6);
    shot("sound");
    page(7, 8);
    shot("messages");

    // Sound: the first setting (audio.enabled) has the focus; Space switches it
    page(7, 6);
    key(SDLK_SPACE);
    expect("Sound: Space switches audio.enabled off", [] { return settingIs("audio.enabled", "off"); });
    key(SDLK_DOWN);  // audio.volume
    key(SDLK_SPACE); // adjust it
    keys(SDLK_RIGHT, 3);
    key(SDLK_SPACE); // done
    expect("Sound: Space, then the right arrow turns the volume up",
           [] { return std::atoi(settings_get("audio.volume")) > 50; });
    shot("sound_changed");

    // Controllers: joy1.a: choose (replace), add one, clear, back to the default
    page(6, 2);
    key(SDLK_SPACE);
    shot("capture");
    key(SDLK_x);
    expect("Controllers: a pressed key replaces joy1.a", [] { return settingIs("joy1.a", "key:x"); });
    key(SDLK_RIGHT); // "+"
    key(SDLK_SPACE);
    key(SDLK_PERIOD);
    expect("Controllers: + adds a key", [] { return settingIs("joy1.a", "key:x, key:period"); });
    key(SDLK_SPACE);
    key(SDLK_ESCAPE);
    expect("Controllers: Esc cancels choosing", [] { return settingIs("joy1.a", "key:x, key:period"); });
    key(SDLK_RIGHT); // Clear
    key(SDLK_SPACE);
    expect("Controllers: Clear", [] { return settingIs("joy1.a", "none"); });
    key(SDLK_RIGHT); // Default
    key(SDLK_SPACE);
    expect("Controllers: Default", [] { return settingIs("joy1.a", "key:period, pad1:b"); });
    // joy1.b: a gamepad button
    key(SDLK_DOWN);
    keys(SDLK_LEFT, 3);
    key(SDLK_SPACE);
    button(SDL_CONTROLLER_BUTTON_X);
    expect("Controllers: a gamepad button replaces joy1.b", [] { return settingIs("joy1.b", "pad1:x"); });

    // Hotkeys: hotkey.menu = Shift+F5 (keys only, with the modifiers)
    page(2, 3);
    key(SDLK_SPACE);
    key(SDLK_F5, KMOD_LSHIFT);
    expect("Hotkeys: Shift+F5 for the menu", [] { return settingIs("hotkey.menu", "key:shift+f5"); });
    key(SDLK_SPACE);
    button(SDL_CONTROLLER_BUTTON_A);
    key(SDLK_F6);
    expect("Hotkeys: gamepad buttons are not keys (ignored)", [] { return settingIs("hotkey.menu", "key:f6"); });

    // Logging: log.level from a list
    page(3, 7);
    key(SDLK_SPACE); // open the list (info is selected)
    key(SDLK_DOWN);
    key(SDLK_SPACE); // debug
    expect("Logging: log.level chosen from the list", [] { return settingIs("log.level", "debug"); });
    shot("messages_changed");

    // Quit (Esc to the sidebar, then down past Logging, Cartridge and Game Genie)
    page(6, 9);
    fe_action action = runMenu(false);
    check("Quit returns FE_QUIT", action == FE_QUIT);
    check("the changes were saved when the menu closed", fileHasSetting(saveTo.c_str(), "audio.enabled", "off") &&
                                                             fileHasSetting(saveTo.c_str(), "hotkey.menu", "key:f6") &&
                                                             fileHasSetting(saveTo.c_str(), "joy1.b", "pad1:x") &&
                                                             fileHasSetting(saveTo.c_str(), "log.level", "debug"));
    check("... into a new file with all the documentation", fileHas(saveTo.c_str(), "# --- controllers ---"));
    char error[200];
    settings_set_key("log.level", "info", error, sizeof(error));

    // --- 2. A game (pretend): the Game page; Esc goes back to the game ------------------------------------------
    shot("game");
    key(SDLK_ESCAPE);
    check("with a game, Esc resumes it", runMenu(true) == FE_RESUME);
    at(4, [] {});
    key(SDLK_F6); // hotkey.menu, changed above
    check("the menu's own key resumes the game", runMenu(true) == FE_RESUME);

    // --- 2b. The Game Genie page (with a game): N opens it with the focus on "Game Genie on"; a code ticked/unticked
    fe_gg_add("SXIOPO", "test code"); // a code in the game's list (adding ticks it: Game Genie on)
    fe_gg_set_enabled(false);
    key(SDLK_n);
    shot("gamegenie");
    key(SDLK_SPACE);
    expect("Game Genie page: Space switches Game Genie on", [] { return fe_gg_enabled(); });
    key(SDLK_DOWN); // the first code of "Your codes"
    key(SDLK_SPACE);
    expect("Game Genie page: Space unticks the code", [] {
        char code[16], description[128];
        bool on = true;
        return fe_gg_entry(0, code, sizeof(code), description, sizeof(description), &on) && !on &&
               std::string(code) == "SXIOPO";
    });
    keys(SDLK_ESCAPE, 2); // to the sidebar, back to the game
    check("Game Genie page: Esc, Esc resumes the game", runMenu(true) == FE_RESUME);
    fe_gg_delete(0);
    fe_gg_set_enabled(false);

    // --- 3. The gamepad: down to Controllers, A, B (back to the sidebar), B (back to the game) -------------------
    button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    button(SDL_CONTROLLER_BUTTON_A);
    shot("gamepad");
    expect("gamepad: A on Controllers shows the page", [] { return true; });
    button(SDL_CONTROLLER_BUTTON_B);
    expect("gamepad: B in a page goes back to the sidebar (the menu stays)", [] { return true; });
    button(SDL_CONTROLLER_BUTTON_B);
    check("gamepad: B in the sidebar resumes the game", runMenu(true) == FE_RESUME);
    button(SDL_CONTROLLER_BUTTON_GUIDE);
    check("gamepad: the guide button resumes the game", runMenu(true) == FE_RESUME);
    // menu.gamepad_button = select+start: controller 1's Select and Start (pad1:back, pad1:start), together
    settings_set_key("menu.gamepad_button", "select+start", error, sizeof(error));
    at(4, [] {
        SDL_JoystickSetVirtualButton(pad, SDL_CONTROLLER_BUTTON_BACK, 1);
        SDL_JoystickUpdate();
    });
    button(SDL_CONTROLLER_BUTTON_START);
    at(1, [] {
        SDL_JoystickSetVirtualButton(pad, SDL_CONTROLLER_BUTTON_BACK, 0);
        SDL_JoystickUpdate();
    });
    check("gamepad: Select+Start resumes the game (menu.gamepad_button = select+start)", runMenu(true) == FE_RESUME);
    settings_set_key("menu.gamepad_button", "guide", error, sizeof(error));

    // Start, then Save on slot 3: the menu closes with FE_SAVE_STATE and slot 3 current
    button(SDL_CONTROLLER_BUTTON_A); // (the Game page)
    keys(SDLK_DOWN, 4);              // Resume.. -> Screenshot -> slot 1 -> 2 -> 3: its Save button
    shot("slots");
    key(SDLK_SPACE);
    fe_action slot = runMenu(true);
    check("Save on slot 3 returns FE_SAVE_STATE with slot 3 current", slot == FE_SAVE_STATE && fe_slot() == 3);

    // --- 4. In a game: nestest runs; Esc (pushed before it starts) opens the menu from inside the emulation ------
    if (romPath != nullptr) {
        copyFile(romPath, "menu_test.nes");
        std::remove("menu_test.ss2");
        settings_set_key("hotkey.menu", "key:esc", error, sizeof(error));
        auto play = [](uint64_t cycles) {
            glb_clock_stop = cycles;
            pushKey(SDLK_ESCAPE); // read by the first frame's input_poll(): the menu opens in the emulation
            char rom[] = "menu_test.nes";
            if (!emu_power_on(rom)) {
                check("nestest starts", false);
                return;
            }
            sched_enter(cpu_get_thread());
            emu_power_off();
        };
        // Save to slot 2 from the menu: the menu closes, the game goes on and saves at the end of its frame
        key(SDLK_SPACE); // the Game page
        shot("ingame");
        keys(SDLK_DOWN, 3); // Screenshot, slot 1, slot 2: Save
        key(SDLK_SPACE);
        play(1000000);
        script.clear();
        scriptFrame = 0;
        check("in a game: Save on slot 2 saved the game (menu_test.ss2)", fileHas("menu_test.ss2", "ONESTATE"));
        // Load it again
        uint32_t loads = savestate_load_count();
        key(SDLK_SPACE);
        keys(SDLK_DOWN, 3);
        key(SDLK_RIGHT); // Load
        key(SDLK_SPACE);
        play(3000000);
        script.clear();
        scriptFrame = 0;
        check("in a game: Load on slot 2 loaded it", savestate_load_count() == loads + 1);
        // The game is in the recently opened list; key 1 starts it again
        std::string newest = fe_recent_count(FE_RECENT_GAMES) > 0 ? fe_recent(FE_RECENT_GAMES, 0) : "";
        check("recent games: the game played is the newest",
              fe_recent_count(FE_RECENT_GAMES) == 1 && newest.size() >= 13 &&
                  newest.compare(newest.size() - 13, 13, "menu_test.nes") == 0);
        shot("recent");
        key(SDLK_1);
        fe_action again = runMenu(true);
        check("recent games: key 1 opens the newest", again == FE_OPEN_ROM && newest == fe_next_rom());

        // Lua scripts: the API, then the menu's Lua page while the game runs
        FILE *lua = std::fopen("menu_test.lua", "w");
        std::fputs("local n = 0\n"
                   "emu.addEventCallback(function()\n"
                   "  n = n + 1\n"
                   "  if n == 5 then local f = io.open('menu_test_lua.txt', 'w'); f:write('ran'); f:close() end\n"
                   "  emu.drawRectangle(8, 8, 40, 20, 0x00FF0000, true)\n"
                   "end, emu.eventType.endFrame)\n",
                   lua);
        std::fclose(lua);
        lua = std::fopen("menu_test_bad.lua", "w");
        std::fputs("this is not Lua\n", lua);
        std::fclose(lua);
        bool loaded = fe_script_load("menu_test.lua");
        check("Lua: fe_script_load runs a script", loaded && fe_script_count() == 1);
        check("Lua: fe_script_stop ends it", fe_script_stop(0) && fe_script_count() == 0);
        check("Lua: a started script is in the recent list", fe_recent_count(FE_RECENT_SCRIPTS) == 1);
        check("Lua: a script with an error does not start, and says why",
              !fe_script_load("menu_test_bad.lua") && fe_script_count() == 0 &&
                  std::strstr(fe_script_error(), "menu_test_bad.lua") != nullptr);
        std::remove("menu_test_lua.txt");
        key(SDLK_l); // the Lua page
        key(SDLK_1); // the recent script (the bad one did not go into the list)
        shot("lua");
        key(SDLK_ESCAPE); // back to the game
        play(2000000);
        script.clear();
        scriptFrame = 0;
        check("Lua page: key 1 started the recent script, and it ran",
              fe_script_count() == 1 && fileHas("menu_test_lua.txt", "ran"));
        key(SDLK_l);
        key(SDLK_RIGHT); // Stop all
        key(SDLK_SPACE);
        key(SDLK_ESCAPE);
        play(1000000);
        script.clear();
        scriptFrame = 0;
        check("Lua page: Stop all", fe_script_count() == 0);
        std::remove("menu_test.lua");
        std::remove("menu_test_bad.lua");
        std::remove("menu_test_lua.txt");
        std::remove("menu_test.nes");
        std::remove("menu_test.ss2");
    } else {
        std::printf("      (no ROM given: the in-game part is skipped)\n");
    }

    // --- 5. Accelerator keys -------------------------------------------------------------------------------------
    key(SDLK_r);
    check("key R resumes the game", runMenu(true) == FE_RESUME);
    key(SDLK_e);
    check("key E resets", runMenu(true) == FE_RESET);
    key(SDLK_p);
    check("key P power cycles", runMenu(true) == FE_POWER_CYCLE);
    key(SDLK_r, KMOD_LALT);
    check("Alt+R resumes too", runMenu(true) == FE_RESUME);
    key(SDLK_o);      // the ROM browser
    key(SDLK_ESCAPE); // back to the Game page
    key(SDLK_ESCAPE); // back to the game
    check("key O opens the browser (Esc, Esc: back to the game)", runMenu(true) == FE_RESUME);
    key(SDLK_s);     // the Sound page
    key(SDLK_SPACE); // its first setting: audio.enabled (off since part 1)
    key(SDLK_q);
    check("key S chooses the Sound page, key Q quits", runMenu(false) == FE_QUIT && settingIs("audio.enabled", "on"));
    key(SDLK_e);
    key(SDLK_q);
    check("without a game, E does nothing (Q quits)", runMenu(false) == FE_QUIT);

    // --- 6. Left / Right between the sidebar and the page --------------------------------------------------------
    // Right on any sidebar entry goes to the shown page's first item; Left on a page's leftmost item goes back to
    // the sidebar's current entry (Dear ImGui alone often found no item beside the entry)
    key(SDLK_s);     // the Sound page, focus on audio.enabled (on)
    key(SDLK_LEFT);  // the sidebar: Sound
    key(SDLK_DOWN);  // Logging: its page shows
    key(SDLK_UP);    // Sound again
    key(SDLK_RIGHT); // Sound's first item
    key(SDLK_SPACE);
    expect("Right from a sidebar entry goes to the page's first item",
           [] { return settingIs("audio.enabled", "off"); });
    key(SDLK_DOWN); // the volume slider
    at(1, [] {
        focusSeen.clear();
        trackFocus = true;
    });
    key(SDLK_LEFT); // nothing left of it in the page: the sidebar (Sound)
    at(6, [] {
        trackFocus = false;
        check("Left goes straight to the sidebar's entry (no other entry on the way)", focusSeen.size() == 2);
    });
    keys(SDLK_UP, 2); // Menu: its page shows
    key(SDLK_RIGHT);  // menu.hint
    key(SDLK_SPACE);
    expect("Left from the page goes to the sidebar's current entry",
           [] { return settingIs("menu.hint", "off") && settingIs("audio.enabled", "off"); });
    key(SDLK_SPACE);  // (menu.hint on again)
    key(SDLK_ESCAPE); // the sidebar (Menu)
    keys(SDLK_UP, 4); // Start: its page shows
    key(SDLK_RIGHT);  // Open a game...
    key(SDLK_SPACE);  // the browser
    shot("browser");
    key(SDLK_ESCAPE); // back to Start
    key(SDLK_q);
    check("Right from Start reaches Open a game... (the browser opened, Esc came back)", runMenu(false) == FE_QUIT);
    settings_set_key("audio.enabled", "on", error, sizeof(error));

    std::printf("gui_test: %d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILED");
    input_shutdown();
    video_shutdown();
    return failures == 0 ? 0 : 1;
}
