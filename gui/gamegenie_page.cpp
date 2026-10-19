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
// The Game Genie page: the game's codes, ticked or not, Game Genie on/off, adding a
// code, and the known codes for the game. Everything through frontend.h (fe_gg_*); changes are saved at once.
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "frontend.h"
#include "imgui.h"
#include "menu.hpp"

namespace gui {

namespace {

// Code letters only, upper case (a code is typed as on the cartridge's booklet)
int codeFilter(ImGuiInputTextCallbackData *data)
{
    const char letters[] = "APZLGITYEOXUKSVN";
    int c = std::toupper(data->EventChar);
    for (const char *p = letters; *p != '\0'; p++) {
        if (*p == c) {
            data->EventChar = static_cast<ImWchar>(c);
            return 0;
        }
    }
    return 1; // (anything else is dropped)
}

bool containsNoCase(const std::string &text, const std::string &word)
{
    auto low = [](std::string s) {
        for (char &c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    return word.empty() || low(text).find(low(word)) != std::string::npos;
}

void drawYourCodes(Menu &menu)
{
    ImGui::SeparatorText("Your codes");
    int count = fe_gg_count();
    if (count == 0) {
        ImGui::TextDisabled("None yet: tick a known code below, or add your own.");
        return;
    }
    if (!ImGui::BeginTable("codes", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        return;
    }
    ImGui::TableSetupColumn("on", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("description", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("delete", ImGuiTableColumnFlags_WidthFixed);
    for (int i = 0; i < count; i++) {
        char code[16], description[128];
        bool on = false;
        if (!fe_gg_entry(i, code, sizeof(code), description, sizeof(description), &on)) {
            continue;
        }
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Checkbox(code, &on)) {
            fe_gg_set_on(i, on);
        }
        char what[64];
        fe_gg_describe(code, what, sizeof(what));
        noteHelp(menu, what);
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(description[0] != '\0' ? description : "(no description)");
        ImGui::TableNextColumn();
        bool remove = ImGui::SmallButton("Delete");
        noteHelp(menu, "Take this code out of the list");
        ImGui::PopID();
        if (remove) {
            fe_gg_delete(i);
            menu.message = std::string("Deleted ") + code;
            break; // (the list changed)
        }
    }
    ImGui::EndTable();
}

void drawAddCode(Menu &menu)
{
    ImGui::SeparatorText("Add a code");
    char what[96];
    bool valid = fe_gg_describe(menu.ggCode, what, sizeof(what));
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("WWWWWWWWW").x);
    ImGui::InputText("##code", menu.ggCode, sizeof(menu.ggCode), ImGuiInputTextFlags_CallbackCharFilter, codeFilter);
    noteHelp(menu, "The code: 6 or 8 letters, as in the Game Genie booklet");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Add").x - ImGui::GetStyle().FramePadding.x * 4.0f);
    ImGui::InputTextWithHint("##description", "description", menu.ggDescription, sizeof(menu.ggDescription));
    noteHelp(menu, "What the code does (for you: optional)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Add")) {
        if (fe_gg_add(menu.ggCode, menu.ggDescription)) {
            menu.message = std::string("Added ") + menu.ggCode + " (ticked)";
            menu.ggCode[0] = menu.ggDescription[0] = '\0';
        } else {
            menu.message = "The list is full";
        }
    }
    ImGui::EndDisabled();
    noteHelp(menu, "Put the code in the list, ticked");
    if (menu.ggCode[0] != '\0') {
        ImGui::TextDisabled("%s", what);
    }
}

void drawKnownCodes(Menu &menu)
{
    int count = fe_gg_db_count();
    if (count == 0) {
        ImGui::SeparatorText("Known codes");
        ImGui::TextDisabled("None known for this game.");
        return;
    }
    char title[64];
    std::snprintf(title, sizeof(title), "Known codes (%d)", count);
    ImGui::SeparatorText(title);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "filter: lives, energy, jump...", menu.ggFilter, sizeof(menu.ggFilter));
    noteHelp(menu, "Show only the codes whose description has this");
    std::vector<int> shown;
    std::vector<std::string> codes(static_cast<size_t>(count)), descriptions(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
        char c[128], d[128];
        fe_gg_db_entry(i, c, sizeof(c), d, sizeof(d));
        codes[static_cast<size_t>(i)] = c;
        descriptions[static_cast<size_t>(i)] = d;
        if (containsNoCase(d, menu.ggFilter) || containsNoCase(c, menu.ggFilter)) {
            shown.push_back(i);
        }
    }
    ImGuiListClipper clipper; // (Super Mario Bros. has 641)
    clipper.Begin(static_cast<int>(shown.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
            int i = shown[static_cast<size_t>(row)];
            ImGui::PushID(i);
            bool on = fe_gg_db_state(i) == 2;
            std::string label = descriptions[static_cast<size_t>(i)] + "##k";
            if (ImGui::Checkbox(label.c_str(), &on)) {
                fe_gg_db_set(i, on);
                menu.message = (on ? "Ticked " : "Unticked ") + codes[static_cast<size_t>(i)];
            }
            noteHelp(menu, codes[static_cast<size_t>(i)].c_str());
            ImGui::PopID();
        }
    }
}

} // namespace

void drawGameGeniePage(Menu &menu)
{
    if (!menu.gameRunning) {
        ImGui::TextWrapped("Game Genie codes are for the game that is running: start one first.");
        menu.focusPage = false;
        return;
    }
    bool enabled = fe_gg_enabled();
    if (ImGui::Checkbox("Game Genie on for this game", &enabled)) {
        fe_gg_set_enabled(enabled);
        menu.message = enabled ? "Game Genie on" : "Game Genie off (the codes stay listed)";
    }
    focusIfNew(menu);
    noteHelp(menu, "Like plugging the Game Genie in: the ticked codes are applied (ticking a code switches it on)");
    const char *game = fe_gg_game();
    ImGui::TextDisabled("%s", game[0] != '\0' ? game : "A game the code database does not know");
    if (ImGui::IsItemHovered() && fe_gg_file()[0] != '\0') {
        menu.help = std::string("Remembered in ") + fe_gg_file();
    }
    drawYourCodes(menu);
    drawAddCode(menu);
    drawKnownCodes(menu);
}

} // namespace gui
