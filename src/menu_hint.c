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
// The hint that the game window has a menu (see menu_hint.h)
#include "menu_hint.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "frontend.h"
#include "settings.h"
#include "video.h"

#define HINT_MS 4000 // the start-up hint stays this long

static bool hint_on = true; // menu.hint

// --- settings ----------------------------------------------------------------------------------------------------

static bool apply_menu_hint(const setting *s, const char *value, char *error, size_t error_size)
{
    (void)s;
    if (error_size > 0) { // (never fails)
        error[0] = '\0';
    }
    settings_parse_bool(value, &hint_on);
    return true;
}

static const setting menu_hint_table[] = {
    {"menu.hint", SETTING_BOOL, "on", "Start-up hint", NULL,
     "when a game starts, show which key opens the menu for a few seconds", 0, 0, NULL, 0, apply_menu_hint, 0},
};

const setting_group *menu_hint_settings(void)
{
    static const setting_group group = {"menu", "Menu", "How the game window shows that it has a menu.",
                                        menu_hint_table, (int)(sizeof(menu_hint_table) / sizeof(menu_hint_table[0]))};
    return &group;
}

// --- the hint ----------------------------------------------------------------------------------------------------

// The menu key as people write it: hotkey.menu "key:esc, key:f11" -> "Esc"; "key:shift+f1" -> "Shift+F1"
static void menu_key_name(char *name, size_t size)
{
    const char *keys = settings_get("hotkey.menu");
    snprintf(name, size, "%s", "Esc");
    if (keys == NULL || strncmp(keys, "key:", 4) != 0) {
        return;
    }
    snprintf(name, size, "%.*s", (int)strcspn(keys + 4, ", "), keys + 4);
    bool start = true; // capitalise each part: shift+f1 -> Shift+F1
    for (char *c = name; *c != '\0'; c++) {
        *c = start ? (char)toupper((unsigned char)*c) : *c;
        start = (*c == '+');
    }
}

void menu_hint_game_started(void)
{
    if (!hint_on || !fe_menu_available()) {
        return;
    }
    char key[32];
    char text[48];
    menu_key_name(key, sizeof(key));
    snprintf(text, sizeof(text), "%s: menu", key);
    video_overlay_for(HINT_MS, text);
}
