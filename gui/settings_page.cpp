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
// The settings pages: one per settings group (settings.h), a widget per setting type. A change is put into effect
// at once (settings_set()); the emulator saves the changes into one.cfg when the menu closes.
#include <SDL.h>

#include <algorithm>
#include <cstdlib>
#include <string>

#include "frontend.h"
#include "menu.hpp"
// (after imgui.h, which menu.hpp includes)
#include "imgui_stdlib.h"
#include "settings.h"

namespace gui {

namespace {

constexpr unsigned kCaptureTimeoutMs = 10000;

// one.cfg's descriptions are wrapped for the file: one line for the menu
std::string oneLine(const char *text)
{
    std::string s = text != nullptr ? text : "";
    for (char &c : s) {
        if (c == '\n') {
            c = ' ';
        }
    }
    return s;
}

std::string helpOf(const setting &s)
{
    std::string help = oneLine(s.description);
    if ((s.flags & SETTING_RESTART) != 0) {
        help += help.empty() ? "" : " ";
        help += "(* takes effect the next time ONE starts)";
    }
    return help;
}

void set(Menu &menu, const setting &s, const std::string &value)
{
    char error[300];
    if (!settings_set(&s, value.c_str(), error, sizeof(error))) {
        menu.message = std::string(s.key) + ": " + error;
    } else {
        menu.message.clear();
    }
}

// What a choice is shown as ("" means "not set": for log.CATEGORY, "as log.level")
const char *choiceLabel(const char *choice)
{
    return choice[0] != '\0' ? choice : "(as above)";
}

void boolWidget(Menu &menu, const setting &s)
{
    bool on = false;
    settings_parse_bool(settings_value(&s), &on);
    if (ImGui::Checkbox("##value", &on)) {
        set(menu, s, on ? "on" : "off");
    }
    focusIfNew(menu);
}

void intWidget(Menu &menu, const setting &s, float width)
{
    int value = std::atoi(settings_value(&s));
    ImGui::SetNextItemWidth(width);
    if (ImGui::SliderInt("##value", &value, s.min, s.max, "%d", ImGuiSliderFlags_AlwaysClamp)) {
        set(menu, s, std::to_string(value));
    }
    focusIfNew(menu);
}

void choiceWidget(Menu &menu, const setting &s, float width)
{
    const std::string current = settings_value(&s); // (a copy: choosing one below replaces the value)
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##value", choiceLabel(current.c_str()))) {
        for (const char *const *c = s.choices; *c != nullptr; c++) {
            bool selected = current == *c;
            if (ImGui::Selectable(choiceLabel(*c), selected)) {
                set(menu, s, *c);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    focusIfNew(menu);
}

// Text and file names: changed when the editing ends (Enter, or the focus leaves)
void textWidget(Menu &menu, const setting &s, float width)
{
    auto edit = menu.textEdits.find(s.key);
    if (edit == menu.textEdits.end()) {
        edit = menu.textEdits.emplace(s.key, settings_value(&s)).first;
    }
    ImGui::SetNextItemWidth(width);
    ImGui::InputText("##value", &edit->second);
    focusIfNew(menu);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        set(menu, s, edit->second);
        menu.textEdits.erase(edit); // (shows the setting's value again: unchanged if it was refused)
    }
}


// The dialog is over: the focus goes back to the button that opened it
void endCapture(Menu &menu)
{
    menu.refocus = menu.capture.target;
    menu.refocusAdd = menu.capture.append;
    menu.capture.target = nullptr;
}

// After the "press a key" dialog has closed: the focus back on the button that opened it (the item just drawn)
void refocusIf(Menu &menu, const setting &s, bool add)
{
    if (menu.refocus == &s && menu.refocusAdd == add && menu.capture.target == nullptr &&
        !ImGui::IsPopupOpen("press")) {
        focusLastItem();
        menu.refocus = nullptr;
    }
}

// Inputs and hotkeys: the current ones on a button that asks for a new one; + adds one; Clear; Default
void inputsWidget(Menu &menu, const setting &s, float width)
{
    std::string value = settings_value(&s);
    if (value.empty()) {
        value = "none";
    }
    float small = ImGui::GetFrameHeight() * 1.2f;
    float buttons = small + ImGui::CalcTextSize("Clear").x + ImGui::CalcTextSize("Default").x +
                    ImGui::GetStyle().FramePadding.x * 4.0f + ImGui::GetStyle().ItemSpacing.x * 3.0f;
    bool keys = s.type == SETTING_KEYS;
    float valueWidth = std::max(width - buttons, ImGui::GetFrameHeight() * 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    if (ImGui::Button((value + "##set").c_str(), ImVec2(valueWidth, 0.0f))) {
        menu.capture = Capture{&s, false, keys, SDL_GetTicks()};
    }
    focusIfNew(menu);
    refocusIf(menu, s, false);
    ImGui::PopStyleVar();
    noteHelp(menu, keys ? "Choose the key (hold Shift, Ctrl or Alt with it if wanted): it replaces these"
                        : "Choose the key or gamepad input: it replaces these");
    ImGui::SameLine();
    if (ImGui::Button("+", ImVec2(small, 0.0f))) {
        menu.capture = Capture{&s, true, keys, SDL_GetTicks()};
    }
    refocusIf(menu, s, true);
    noteHelp(menu, keys ? "Add a key" : "Add a key or gamepad input (any of them presses the button)");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        set(menu, s, "none");
    }
    noteHelp(menu, "No inputs");
    ImGui::SameLine();
    if (ImGui::Button("Default")) {
        set(menu, s, s.default_value);
    }
    noteHelp(menu, (std::string("Back to the default: ") + s.default_value).c_str());
}

} // namespace

void drawSettingsPage(Menu &menu, const setting_group &group)
{
    float labelWidth = 0.0f;
    float widgetWidth = 0.0f;
    for (int i = 0; i < group.count; i++) {
        const setting &s = group.settings[i];
        if ((s.flags & SETTING_NO_MENU) != 0) {
            continue; // (one.cfg only)
        }
        if (i == 0 || s.section != nullptr) {
            // the labels of a section line up (each section has a label column of its own)
            labelWidth = 0.0f;
            for (int j = i; j < group.count && (j == i || group.settings[j].section == nullptr); j++) {
                if ((group.settings[j].flags & SETTING_NO_MENU) == 0) {
                    labelWidth = std::max(labelWidth, ImGui::CalcTextSize(group.settings[j].label).x);
                }
            }
            labelWidth += ImGui::CalcTextSize(" * ").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
            widgetWidth = ImGui::GetContentRegionAvail().x - labelWidth;
        }
        if (s.section != nullptr) {
            ImGui::SeparatorText(s.section);
        }
        ImGui::PushID(s.key);
        ImGui::AlignTextToFramePadding();
        if ((s.flags & SETTING_RESTART) != 0) {
            ImGui::Text("%s *", s.label);
        } else {
            ImGui::TextUnformatted(s.label);
        }
        ImGui::SameLine(labelWidth);
        switch (s.type) {
        case SETTING_BOOL: boolWidget(menu, s); break;
        case SETTING_INT: intWidget(menu, s, widgetWidth); break;
        case SETTING_CHOICE: choiceWidget(menu, s, widgetWidth * 0.5f); break;
        case SETTING_TEXT:
        case SETTING_FILE: textWidget(menu, s, widgetWidth); break;
        case SETTING_INPUTS:
        case SETTING_KEYS: inputsWidget(menu, s, widgetWidth); break;
        }
        if (s.type == SETTING_INT) {
            noteHelp(menu, (helpOf(s) + "  [to change: Space or gamepad A, then left/right]").c_str());
        } else if (s.type != SETTING_INPUTS && s.type != SETTING_KEYS) {
            noteHelp(menu, helpOf(s).c_str());
        }
        ImGui::PopID();
    }
}

void drawCapture(Menu &menu)
{
    Capture &capture = menu.capture;
    if (capture.target != nullptr && SDL_GetTicks() - capture.startedMs > kCaptureTimeoutMs) {
        endCapture(menu);
        menu.message = "Nothing was pressed: unchanged";
    }
    if (capture.target != nullptr && !ImGui::IsPopupOpen("press")) {
        ImGui::OpenPopup("press");
    }
    ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("press", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoMove)) {
        if (capture.target == nullptr) {
            ImGui::CloseCurrentPopup();
        } else {
            const setting &s = *capture.target;
            ImGui::Text("%s: %s", s.key, s.label);
            ImGui::Spacing();
            ImGui::TextUnformatted(capture.keysOnly ? "Press the key, with Shift, Ctrl or Alt if wanted."
                                                    : "Press a key, or a button or stick of a gamepad.");
            unsigned left = (kCaptureTimeoutMs - (SDL_GetTicks() - capture.startedMs) + 999) / 1000;
            ImGui::TextDisabled("Esc: cancel (%us)", left);
        }
        ImGui::EndPopup();
    }
}

bool captureEvent(Menu &menu, const void *sdlEvent)
{
    const auto *event = static_cast<const SDL_Event *>(sdlEvent);
    Capture &capture = menu.capture;
    if (event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_ESCAPE &&
        (event->key.keysym.mod & (KMOD_SHIFT | KMOD_CTRL | KMOD_ALT)) == 0) {
        endCapture(menu);
        menu.message = "Cancelled: unchanged";
        return true;
    }
    char name[96];
    if (!fe_input_name(event, capture.keysOnly, name, sizeof(name))) {
        return false;
    }
    const setting &s = *capture.target;
    endCapture(menu);
    std::string old = settings_value(&s);
    std::string value = name;
    if (capture.append && !old.empty() && old != "none") {
        // (no duplicates: the same input twice does nothing)
        std::string items = ", " + old + ",";
        value = items.find(std::string(" ") + name + ",") != std::string::npos ? old : old + ", " + name;
    }
    set(menu, s, value);
    if (menu.message.empty()) {
        menu.message = std::string(s.key) + " = " + value;
    }
    return true;
}

} // namespace gui
