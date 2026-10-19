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
#include "settings.hpp"
#include "debugger/config.hpp"
#include <SDL.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace {

struct SettingInfo {
    const char *key;
    const char *defaultValue;
    const char *description;
    bool Settings::*flag;        // on/off settings
    std::string Settings::*text; // or a gamepad button's name
};

// docs/quickref/extract.sh lists these in the quick reference
const SettingInfo kSettings[] = {
    {"shadows", "off", "drop shadows under the windows (on/off)", &Settings::shadows, nullptr},
    {"handback_button", "none", "gamepad button that stops the game like Ctrl+D (guide, back, misc1...; none)", nullptr,
     &Settings::handbackButton},
};

bool parseBool(std::string text, bool &out)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (text == "on" || text == "yes" || text == "true" || text == "1") {
        out = true;
    } else if (text == "off" || text == "no" || text == "false" || text == "0") {
        out = false;
    } else {
        return false;
    }
    return true;
}

} // namespace

Settings &settings()
{
    static Settings s = [] {
        Settings defaults;
        for (const SettingInfo &info : kSettings) {
            if (info.flag != nullptr) {
                parseBool(info.defaultValue, defaults.*info.flag);
            }
        }
        return defaults;
    }();
    return s;
}

void applySettings(const Config &config, std::vector<std::string> &messages)
{
    for (const SettingInfo &info : kSettings) {
        const std::string *value = config.get(info.key);
        if (value && info.text != nullptr) {
            std::string name = *value;
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name == "none" || name.empty()) {
                name.clear();
            } else if (SDL_GameControllerGetButtonFromString(name.c_str()) == SDL_CONTROLLER_BUTTON_INVALID) {
                messages.push_back(config.path() + ":" + std::to_string(config.line(info.key)) + ": " + info.key +
                                   ": no gamepad button '" + *value +
                                   "' (guide, back, start, misc1, paddle1...; none)");
                continue;
            }
            settings().*info.text = name;
        } else if (value && !parseBool(*value, settings().*info.flag)) {
            messages.push_back(config.path() + ":" + std::to_string(config.line(info.key)) + ": " + info.key +
                               ": expected on or off, not '" + *value + "'");
        }
    }
}

bool isHandbackButton(const void *sdlEvent)
{
    const auto *event = static_cast<const SDL_Event *>(sdlEvent);
    const std::string &button = settings().handbackButton;
    if (button.empty() || event->type != SDL_CONTROLLERBUTTONDOWN) {
        return false;
    }
    const char *name =
        SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(event->cbutton.button));
    return name != nullptr && button == name;
}

std::string settingsConfigText()
{
    std::string text;
    for (const SettingInfo &info : kSettings) {
        std::string line = std::string(info.key) + " = " + info.defaultValue;
        text += line + std::string(line.size() < 52 ? 52 - line.size() : 1, ' ') + "# " + info.description + "\n";
    }
    return text;
}
