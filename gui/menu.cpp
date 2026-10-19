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
// The menu of the game window: Dear ImGui drawn over the paused game, with the mouse, the keyboard or a gamepad.
//
// It runs while the emulation waits (frontend.h): fe_run_menu() calls runMenu(), which has the window to itself
// until the menu closes. A Dear ImGui context is made each time the menu opens and destroyed when it closes, so
// nothing of it outlives the menu (the window and renderer belong to the emulator).
#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "frontend.h"
#include "gui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "imgui_internal.h" // ImGui::SetFocusID(): focus without activating (SetKeyboardFocusHere() activates)
#include "settings.h"
#include "menu.hpp"

namespace gui {


namespace {

gui_frame_hook frameHook = nullptr;

std::string baseName(const std::string &path)
{
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// --- accelerator keys ----------------------------------------------------------------------------------------------
// A letter (or digit) chooses a menu item without moving the focus to it: Q quits, O opens a game, R resumes, E
// resets, P power cycles, G shows the Game page, and each settings page has a letter of its name (underlined, as
// in menus elsewhere); the Start page (no game yet) is T. Recent games are 1-9 and 0. They work with or without Alt; not while a text is being edited, a dialog is open or a key
// is being chosen for a setting.

constexpr const char *kReservedKeys =
    "QORGEPTLN"; // the actions' and the Game / Start / Lua / Game Genie pages' letters

bool acceleratorsOn(const Menu &menu)
{
    const ImGuiIO &io = ImGui::GetIO();
    return menu.capture.target == nullptr && !io.WantTextInput && !io.KeyCtrl && !io.KeySuper &&
           !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !ImGui::IsAnyItemActive();
}

ImGuiKey keyOf(char key)
{
    if (key >= 'A' && key <= 'Z') {
        return static_cast<ImGuiKey>(ImGuiKey_A + (key - 'A'));
    }
    if (key >= 'a' && key <= 'z') {
        return static_cast<ImGuiKey>(ImGuiKey_A + (key - 'a'));
    }
    if (key >= '0' && key <= '9') {
        return static_cast<ImGuiKey>(ImGuiKey_0 + (key - '0'));
    }
    return ImGuiKey_None;
}

// KEY was pressed this frame (and accelerators are on)
bool accelerator(const Menu &menu, char key)
{
    ImGuiKey k = keyOf(key);
    return k != ImGuiKey_None && acceleratorsOn(menu) && !ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(k, false);
}

// Underline KEY's first appearance in LABEL, drawn with its text starting at TEXT
void underline(const char *label, char key, ImVec2 text)
{
    const char *at = nullptr;
    for (const char *p = label; *p != '\0' && at == nullptr; p++) {
        if (std::toupper(static_cast<unsigned char>(*p)) == std::toupper(static_cast<unsigned char>(key))) {
            at = p;
        }
    }
    if (at == nullptr) {
        return;
    }
    float x = text.x + ImGui::CalcTextSize(label, at).x;
    float width = ImGui::CalcTextSize(at, at + 1).x;
    float y = text.y + ImGui::GetFontSize() - 1.0f;
    float thickness = std::max(1.0f, ImGui::GetFontSize() / 14.0f);
    ImU32 color = ImGui::GetColorU32((ImGui::GetCurrentContext()->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0
                                         ? ImGuiCol_TextDisabled
                                         : ImGuiCol_Text);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, y), ImVec2(x + width, y), color, thickness);
}

// A button with KEY underlined in its label (the key itself is handled by globalAccelerators())
bool keyButton(const char *label, char key)
{
    ImVec2 text = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::Button(label);
    ImVec2 padding = ImGui::GetStyle().FramePadding;
    underline(label, key, ImVec2(text.x + padding.x, text.y + padding.y));
    return pressed;
}

// The settings pages' letters: the first letter of each title that no action and no earlier page has
std::string pageKeys()
{
    std::string taken = kReservedKeys;
    std::string keys;
    for (int i = 0; i < settings_group_count(); i++) {
        char key = 0;
        for (const char *p = settings_group(i)->title; *p != '\0' && key == 0; p++) {
            char c = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
            if (c >= 'A' && c <= 'Z' && taken.find(c) == std::string::npos) {
                key = c;
            }
        }
        keys += key != 0 ? key : ' ';
        taken += key;
    }
    return keys;
}

// The size of everything: 1 for a window up to 600 pixels high, more for bigger ones (fonts stay sharp: the
// default font is a vector font)
float uiScale(SDL_Renderer *renderer)
{
    int w = 0;
    int h = 0;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    return std::clamp(static_cast<float>(h) / 600.0f, 1.0f, 4.0f);
}

void setStyle(float scale)
{
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.08f, 0.88f); // the paused game shows through
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.ScaleAllSizes(scale);
    style.FontScaleMain = scale;
}

// A sidebar entry: the cursor on it shows its page; choosing it (or its KEY) also moves the focus there
void sidebarEntry(Menu &menu, const char *label, int page, char key)
{
    bool selected = menu.page == page || (menu.page == kPageBrowser && page == browserParent(menu));
    ImVec2 text = ImGui::GetCursorScreenPos();
    bool chosen = ImGui::Selectable(label, selected);
    underline(label, key, text);
    if (chosen || accelerator(menu, key)) {
        menu.page = page;
        menu.focusPage = true;
        menu.message.clear();
    } else if (ImGui::IsItemFocused() && !selected && !menu.focusSidebar) {
        menu.page = page; // the page follows the sidebar's cursor (Space / Right then go into it)
        menu.message.clear();
    }
    menu.sidebarFocused = menu.sidebarFocused || ImGui::IsItemFocused();
    if (selected && menu.focusSidebar) {
        focusLastItem();
        menu.focusSidebar = false;
    }
}

void finish(Menu &menu, fe_action action)
{
    menu.result = action;
    menu.done = true;
}

// A sidebar item that does something (KEY underlined)
bool sidebarAction(Menu &menu, const char *label, char key)
{
    ImVec2 text = ImGui::GetCursorScreenPos();
    bool chosen = ImGui::Selectable(label);
    underline(label, key, text);
    menu.sidebarFocused = menu.sidebarFocused || ImGui::IsItemFocused();
    return chosen;
}

void drawSidebar(Menu &menu)
{
    ImGui::TextDisabled("ONE %s", fe_version());
    ImGui::Separator();
    sidebarEntry(menu, menu.gameRunning ? "Game" : "Start", kPageGame, menu.gameRunning ? 'G' : 'T');
    sidebarEntry(menu, "Lua scripts", kPageLua, 'L');
    std::string keys = pageKeys();
    for (int i = 0; i < settings_group_count(); i++) {
        sidebarEntry(menu, settings_group(i)->title, i, keys[static_cast<size_t>(i)]);
    }
    sidebarEntry(menu, "Game Genie", kPageGameGenie, 'N'); // (next to Cartridge)
    ImGui::Separator();
    if (menu.gameRunning && sidebarAction(menu, "Resume", 'R')) {
        finish(menu, FE_RESUME);
    }
    if (sidebarAction(menu, "Quit", 'Q')) {
        finish(menu, FE_QUIT);
    }
}

// The actions' keys, wherever the focus is (the pages' keys are in sidebarEntry())
void globalAccelerators(Menu &menu)
{
    if (accelerator(menu, 'Q')) {
        finish(menu, FE_QUIT);
    } else if (accelerator(menu, 'O')) {
        openBrowser(menu);
        menu.browser.focusFirst = true;
    } else if (menu.gameRunning && accelerator(menu, 'R')) {
        finish(menu, FE_RESUME);
    } else if (menu.gameRunning && accelerator(menu, 'E')) {
        finish(menu, FE_RESET);
    } else if (menu.gameRunning && accelerator(menu, 'P')) {
        finish(menu, FE_POWER_CYCLE);
    }
}

// Recently opened games or scripts: 1-9 and 0 (the tenth) choose one. Returns the path chosen ("" if none).
std::string drawRecent(Menu &menu, fe_recent_list list)
{
    int count = fe_recent_count(list);
    if (count == 0) {
        return "";
    }
    ImGui::SeparatorText(list == FE_RECENT_GAMES ? "Recent games" : "Recent scripts");
    std::string result;
    for (int i = 0; i < count && result.empty(); i++) {
        std::string path = fe_recent(list, i);
        char key = static_cast<char>('0' + (i + 1) % 10);
        std::string label = std::string(1, key) + "  " + baseName(path);
        std::error_code ec;
        bool missing = !std::filesystem::exists(path, ec);
        ImGui::PushID(i);
        ImGui::BeginDisabled(missing);
        bool chosen = ImGui::Selectable(label.c_str());
        ImGui::EndDisabled();
        if (missing) {
            ImGui::SameLine();
            ImGui::TextDisabled("(not found)");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || ImGui::IsItemFocused()) {
            menu.help = path + (missing ? " (not found)" : "  (key " + std::string(1, key) + ")");
        }
        ImGui::PopID();
        if (!missing && (chosen || accelerator(menu, key))) {
            result = path;
        }
    }
    ImGui::PushID(list);
    if (ImGui::SmallButton("Clear the list")) {
        fe_recent_clear(list);
    }
    ImGui::PopID();
    noteHelp(menu, list == FE_RECENT_GAMES ? "Forget the recently opened games" : "Forget the recently run scripts");
    return result;
}

void startScript(Menu &menu, const std::string &path)
{
    menu.message =
        fe_script_load(path.c_str()) ? "Started " + baseName(path) : std::string("Error: ") + fe_script_error();
}

// Lua scripts: the running ones (Restart, Stop), Load a script..., the recent ones
void drawLuaPage(Menu &menu)
{
    if (!menu.gameRunning) {
        ImGui::TextWrapped("Lua scripts run with a game: start one first.");
        menu.focusPage = false;
        return;
    }
    if (ImGui::Button("Load a script...")) {
        openBrowser(menu, true);
    }
    focusIfNew(menu);
    noteHelp(menu, "Run a Lua script written for Mesen 2 or FCEUX (it keeps running when another game starts)");
    int count = fe_script_count();
    ImGui::SameLine();
    ImGui::BeginDisabled(count == 0);
    if (ImGui::Button("Stop all")) {
        for (int i = count - 1; i >= 0; i--) {
            fe_script_stop(i);
        }
        menu.message = "Stopped the scripts";
    }
    ImGui::EndDisabled();
    noteHelp(menu, "End every running script (their drawings go)");
    ImGui::SeparatorText("Running");
    if (count == 0) {
        ImGui::TextDisabled("No script is running.");
    }
    for (int i = 0; i < count; i++) {
        std::string path = fe_script_path(i);
        ImGui::PushID(i);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(fe_script_name(i));
        if (ImGui::IsItemHovered()) {
            menu.help = path;
        }
        ImGui::SameLine();
        bool restart = ImGui::Button("Restart");
        noteHelp(menu, "Stop the script and run it again from the start (after editing it, say)");
        ImGui::SameLine();
        bool stop = ImGui::Button("Stop");
        noteHelp(menu, "End this script (its drawings go)");
        ImGui::PopID();
        if (restart || stop) {
            if (!fe_script_stop(i)) {
                menu.message = "The script cannot be stopped now";
            } else if (restart) {
                startScript(menu, path);
            } else {
                menu.message = "Stopped " + baseName(path);
            }
            break; // (the list changed)
        }
    }
    const char *error = fe_script_error();
    if (error[0] != '\0') {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
        ImGui::TextWrapped("Last error: %s", error);
        ImGui::PopStyleColor();
    }
    std::string chosen = drawRecent(menu, FE_RECENT_SCRIPTS);
    if (!chosen.empty()) {
        startScript(menu, chosen);
    }
}

void openRecentGame(Menu &menu, const std::string &path)
{
    if (!path.empty()) {
        fe_set_next_rom(path.c_str());
        finish(menu, FE_OPEN_ROM);
    }
}

void drawSaveStates(Menu &menu)
{
    ImGui::SeparatorText("Save states");
    if (!ImGui::BeginTable("slots", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        return;
    }
    for (int slot = 1; slot <= 9; slot++) {
        ImGui::PushID(slot);
        char when[64];
        bool saved = fe_slot_time(slot, when, sizeof(when));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s Slot %d", slot == fe_slot() ? ">" : " ", slot);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(saved ? when : "empty");
        ImGui::TableNextColumn();
        if (ImGui::Button("Save")) {
            fe_set_slot(slot);
            finish(menu, FE_SAVE_STATE);
        }
        noteHelp(menu, "Save the game to this slot (the menu closes; F1 saves to the current slot, '>')");
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!saved);
        if (ImGui::Button("Load")) {
            fe_set_slot(slot);
            finish(menu, FE_LOAD_STATE);
        }
        ImGui::EndDisabled();
        noteHelp(menu, "Go back to the game saved in this slot (F2 loads the current slot, '>')");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void drawGamePage(Menu &menu)
{
    if (!menu.gameRunning) {
        ImGui::TextWrapped("No game is running. Choose one (an iNES .nes file, or a .zip with one):");
        ImGui::TextDisabled("During play, Esc (or the gamepad's menu button) opens this menu again.");
        if (keyButton("Open a game...", 'O')) {
            openBrowser(menu);
        }
        focusIfNew(menu);
        openRecentGame(menu, drawRecent(menu, FE_RECENT_GAMES));
        return;
    }
    ImGui::TextUnformatted(baseName(fe_rom_path()).c_str());
    ImGui::Spacing();
    if (keyButton("Resume", 'R')) {
        finish(menu, FE_RESUME);
    }
    focusIfNew(menu);
    noteHelp(menu, "Back to the game (also Esc, or the menu button of the gamepad)");
    ImGui::SameLine();
    if (keyButton("Reset", 'E')) {
        finish(menu, FE_RESET);
    }
    noteHelp(menu, "Press the RESET button of the NES");
    ImGui::SameLine();
    if (keyButton("Power cycle", 'P')) {
        finish(menu, FE_POWER_CYCLE);
    }
    noteHelp(menu, "Switch the NES off and on again (the game starts from the beginning)");
    ImGui::SameLine();
    if (keyButton("Open a game...", 'O')) {
        openBrowser(menu);
    }
    noteHelp(menu, "Switch to another game (this one is switched off: save first if you want to keep it)");
    if (ImGui::Button("Screenshot")) {
        char saved[1100];
        menu.message = fe_screenshot(saved, sizeof(saved)) ? std::string("Screenshot saved: ") + saved
                                                           : std::string("Cannot write ") + saved;
    }
    noteHelp(menu, "Save the picture as a PNG next to the ROM (without the menu)");
    ImGui::SameLine();
    bool fps = fe_fps_shown();
    if (ImGui::Checkbox("FPS in the title", &fps)) {
        fe_set_fps(fps);
    }
    noteHelp(menu, "Show the frames per second in the window title");
    ImGui::SameLine();
    bool lag = fe_lag_counter_shown();
    if (ImGui::Checkbox("Lag counter", &lag)) {
        fe_set_lag_counter(lag);
    }
    noteHelp(
        menu,
        "Show the lag frame counter on the picture: frames in which the game did not read the controllers (red when the last frame was one)");
    drawSaveStates(menu);
    openRecentGame(menu, drawRecent(menu, FE_RECENT_GAMES));
}

void drawFooter(Menu &menu)
{
    ImGui::Separator();
    if (!menu.message.empty()) {
        ImGui::TextWrapped("%s", menu.message.c_str());
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    if (!menu.help.empty()) {
        ImGui::TextWrapped("%s", menu.help.c_str());
    } else {
        ImGui::TextWrapped("Settings are saved to %s when the menu closes.", settings_path());
    }
    ImGui::PopStyleColor();
}

void draw(Menu &menu)
{
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    menu.help.clear();
    menu.sidebarFocused = false;
    float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    float sidebar = std::max(ImGui::CalcTextSize("Controllers").x * 1.8f, io.DisplaySize.x * 0.2f);
    sidebar = std::max(sidebar, ImGui::CalcTextSize((std::string("ONE ") + fe_version()).c_str()).x +
                                    ImGui::GetStyle().WindowPadding.x * 2.0f); // (the header shows the version)
    ImGui::BeginChild("sidebar", ImVec2(sidebar, -footer), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    menu.sidebarWindow = ImGui::GetCurrentWindow();
    drawSidebar(menu);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("page", ImVec2(0.0f, -footer), ImGuiChildFlags_NavFlattened);
    if (menu.page == kPageGame) {
        drawGamePage(menu);
    } else if (menu.page == kPageLua) {
        drawLuaPage(menu);
    } else if (menu.page == kPageGameGenie) {
        drawGameGeniePage(menu);
    } else if (menu.page == kPageBrowser) {
        drawBrowser(menu);
    } else if (const setting_group *group = settings_group(menu.page)) {
        drawSettingsPage(menu, *group);
        if (std::strcmp(group->name, "messages") == 0) {
            ImGui::SeparatorText("Where the messages go");
            ImGui::TextUnformatted(fe_log_destination());
        }
    }
    ImGui::EndChild();
    drawFooter(menu);
    drawCapture(menu);
    if (!menu.done) {
        globalAccelerators(menu);
    }
    ImGui::End();
}

// Moving between the sidebar and the page. Dear ImGui's own arrow-key navigation goes to the nearest item in the
// arrow's direction, which between two columns of different heights often finds none (a sidebar entry has no page
// item beside it) or a far one. So: Right (or the d-pad's right) in the sidebar always goes to the page's first
// item (navigateBeforeDraw()); a move out of the page is looked at once Dear ImGui has scored the items, before it
// is carried out (navigateAfterDraw()): Left that finds no page item, or a sidebar entry, goes to the sidebar's
// current entry, and Up or Down never leave their column.
bool navAllowed(const Menu &menu)
{
    return menu.capture.target == nullptr && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
           !ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput;
}

void navigateBeforeDraw(Menu &menu)
{
    bool right = ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight);
    if (navAllowed(menu) && menu.sidebarFocused && right) {
        ImGui::NavMoveRequestCancel();
        menu.focusPage = true;
        if (menu.page == kPageBrowser) {
            menu.browser.focusFirst = true;
        }
    }
}

bool inSidebar(const Menu &menu, const ImGuiWindow *window)
{
    for (; window != nullptr; window = window->ParentWindow) {
        if (window == menu.sidebarWindow) {
            return true;
        }
    }
    return false;
}

void navigateAfterDraw(Menu &menu)
{
    ImGuiContext &g = *ImGui::GetCurrentContext();
    if (!g.NavMoveSubmitted || !navAllowed(menu) || menu.focusSidebar || menu.focusPage) {
        return;
    }
    const ImGuiNavItemData *result = g.NavMoveResultLocal.ID != 0   ? &g.NavMoveResultLocal
                                     : g.NavMoveResultOther.ID != 0 ? &g.NavMoveResultOther
                                                                    : nullptr;
    bool toSidebar = result != nullptr && inSidebar(menu, result->Window);
    if (g.NavMoveDir == ImGuiDir_Left && !menu.sidebarFocused && (result == nullptr || toSidebar)) {
        ImGui::NavMoveRequestCancel();
        menu.focusSidebar = true; // the current page's entry
    } else if ((g.NavMoveDir == ImGuiDir_Up || g.NavMoveDir == ImGuiDir_Down) && result != nullptr &&
               toSidebar != menu.sidebarFocused) {
        ImGui::NavMoveRequestCancel(); // (stay at the top / bottom of the column)
    }
}

// Going back: out of the ROM browser to the Game page; from a page to the sidebar; from the sidebar (or at once,
// for Esc: CLOSE) back to the game (if one is running)
void back(Menu &menu, bool close)
{
    if (menu.page == kPageBrowser) {
        menu.page = browserParent(menu);
        menu.focusPage = true;
    } else if (menu.gameRunning && (close || menu.sidebarFocused)) {
        finish(menu, FE_RESUME);
    } else {
        menu.focusSidebar = true;
    }
}

void handleEvent(Menu &menu, SDL_Event &event)
{
    if (fe_window_event(&event)) { // the window was closed
        finish(menu, FE_QUIT);
        return;
    }
    if (menu.capture.target != nullptr) { // all keys and buttons go to the "press a key" dialog
        captureEvent(menu, &event);
        return;
    }
    bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    bool editing = ImGui::GetIO().WantTextInput;
    bool active = ImGui::IsAnyItemActive(); // (Esc first ends adjusting a slider)
    if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE && event.key.repeat == 0 && !popup &&
        !editing && !active) {
        back(menu, true);
        return;
    }
    if (fe_is_menu_button(&event) && menu.gameRunning && !editing) { // the menu's own key or gamepad button
        finish(menu, FE_RESUME);
        return;
    }
    ImGui_ImplSDL2_ProcessEvent(&event);
}

fe_action runMenu(bool gameRunning)
{
    auto *window = static_cast<SDL_Window *>(fe_sdl_window());
    auto *renderer = static_cast<SDL_Renderer *>(fe_sdl_renderer());
    if (window == nullptr || renderer == nullptr) {
        return FE_RESUME;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    setStyle(uiScale(renderer));
    io.Fonts->AddFontDefaultVector();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    Menu menu;
    menu.gameRunning = gameRunning;
    int frame = 0;
    while (!menu.done) {
        SDL_Event event;
        bool have = SDL_WaitEventTimeout(&event, 16) != 0; // (frames keep coming: gamepad navigation polls)
        while (have && !menu.done) {
            handleEvent(menu, event);
            have = SDL_PollEvent(&event) != 0;
        }
        if (menu.done) {
            break;
        }
        if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0) {
            continue;
        }
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        // gamepad B, when it has nothing else to cancel: like Esc
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::IsAnyItemActive() &&
            !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && menu.capture.target == nullptr) {
            back(menu, false);
        }
        navigateBeforeDraw(menu);
        draw(menu);
        navigateAfterDraw(menu);
        ImGui::Render();
        SDL_RenderSetScale(renderer, 1.0f, 1.0f);
        fe_draw_game();
        SDL_RenderSetScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (frameHook != nullptr) {
            frameHook(frame);
        }
        frame++;
        SDL_RenderPresent(renderer);
    }
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    return menu.result;
}

} // namespace

void focusLastItem()
{
    // (FocusItem() does not move the focus between the sidebar and the page: they are flattened child windows of
    // one navigation scope. Setting the focus id itself does.)
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
    ImGui::ScrollToItem();
}

void focusIfNew(Menu &menu)
{
    if (menu.focusPage) {
        focusLastItem();
        menu.focusPage = false;
    }
}

void noteHelp(Menu &menu, const char *text)
{
    if (text != nullptr && (ImGui::IsItemHovered() || ImGui::IsItemFocused())) {
        menu.help = text;
    }
}

} // namespace gui

extern "C" {

void gui_install(void)
{
    fe_set_menu(gui::runMenu);
}

void gui_set_frame_hook(gui_frame_hook hook)
{
    gui::frameHook = hook;
}
}
