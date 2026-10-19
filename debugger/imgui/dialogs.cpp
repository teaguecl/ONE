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
#include "dialogs.hpp"
#include "debugger/theme.hpp"
#include "views.hpp" // (drawShadow())
#include <cstdint>
#include <cstddef>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#include <utility>

namespace fs = std::filesystem;

namespace {

ImVec4 rgb(uint32_t value)
{
    return ImVec4(static_cast<float>((value >> 16) & 0xFF) / 255.0f, static_cast<float>((value >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(value & 0xFF) / 255.0f, 1.0f);
}

// Turbo Vision's gray dialogs (VGA colors): black on light gray, a white frame and title, black-on-green buttons,
// white-on-blue input lines, black-on-cyan lists with the selection in green
class DialogColors {
public:
    DialogColors()
    {
        set(ImGuiCol_PopupBg, 0xAAAAAA);
        set(ImGuiCol_WindowBg, 0xAAAAAA);
        set(ImGuiCol_TitleBg, 0xAAAAAA);
        set(ImGuiCol_TitleBgActive, 0xAAAAAA);
        set(ImGuiCol_Border, 0xFFFFFF);
        set(ImGuiCol_Text, 0x000000);
        set(ImGuiCol_Button, 0x00AA00);
        set(ImGuiCol_ButtonHovered, 0x55FF55);
        set(ImGuiCol_ButtonActive, 0x55FF55);
        set(ImGuiCol_FrameBg, 0x0000AA);
        set(ImGuiCol_FrameBgHovered, 0x0000AA);
        set(ImGuiCol_FrameBgActive, 0x0000AA);
        set(ImGuiCol_ChildBg, 0x00AAAA);
        set(ImGuiCol_Header, 0x00AA00);
        set(ImGuiCol_HeaderHovered, 0x55FFFF);
        set(ImGuiCol_HeaderActive, 0x00AA00);
        set(ImGuiCol_NavCursor, 0xFFFFFF);
        set(ImGuiCol_TextSelectedBg, 0x00AAAA);
        set(ImGuiCol_ScrollbarBg, 0x00AAAA);
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0)); // (Turbo Vision does not dim)
        count++;
        float cell = ImGui::GetFontSize();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(cell * 0.5f, cell * 0.15f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(cell, cell * 0.75f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(cell * 0.6f, cell * 0.5f));
    }
    ~DialogColors()
    {
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(count);
    }
    DialogColors(const DialogColors &) = delete;
    DialogColors &operator=(const DialogColors &) = delete;

private:
    void set(ImGuiCol which, uint32_t value)
    {
        ImGui::PushStyleColor(which, rgb(value));
        count++;
    }
    int count = 0;
};

// An input line: white on blue
void inputColors(bool push)
{
    if (push) {
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(0xFFFFFF));
    } else {
        ImGui::PopStyleColor();
    }
}

// "~Y~es" -> "Yes" and 'y'
std::string withoutTildes(const std::string &label, char *hotkey)
{
    std::string text;
    bool opened = false;
    for (size_t i = 0; i < label.size(); i++) {
        if (label[i] == '~') {
            // the opening '~' is followed by the hotkey letter (the closing one by the rest of the label)
            if (!opened && hotkey != nullptr && i + 1 < label.size()) {
                *hotkey = static_cast<char>(std::tolower(static_cast<unsigned char>(label[i + 1])));
            }
            opened = !opened;
            continue;
        }
        text += label[i];
    }
    return text;
}

// The key of a button's hotkey letter ('y'), pressed this frame, alone or with Alt (as in Turbo Vision)
bool hotkeyPressed(char letter)
{
    const ImGuiIO &io = ImGui::GetIO();
    if (letter < 'a' || letter > 'z' || io.KeyCtrl || io.KeyShift) {
        return false;
    }
    return ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A + (letter - 'a')), false);
}

// A dialog's button, as Turbo Vision draws one: with a black shadow (half a row below, a column at the right; none
// while it is held down). A LABEL with a hotkey letter between '~' ("~Y~es") shows the letter in the theme's
// menu_shortcut color, and pressing the letter presses the button.
bool dialogButton(const std::string &label, float width)
{
    char letter = 0;
    std::string text = withoutTildes(label, &letter);
    bool pressed = ImGui::Button(text.c_str(), ImVec2(width, 0.0f));
    const ImGuiStyle &style = ImGui::GetStyle();
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    if (!ImGui::IsItemActive()) {
        const float column = ImGui::CalcTextSize("M").x, half = std::round((max.y - min.y) * 0.5f);
        const ImU32 black = IM_COL32(0, 0, 0, 0xFF);
        drawList->AddRectFilled(ImVec2(max.x, min.y + half), ImVec2(max.x + column, max.y), black);
        drawList->AddRectFilled(ImVec2(min.x + column, max.y), ImVec2(max.x + column, max.y + half), black);
    }
    const size_t at = label.find('~');
    if (letter != 0 && at != std::string::npos) {
        ImVec2 size = ImGui::CalcTextSize(text.c_str());
        float left = min.x + style.FramePadding.x;
        float x = left + std::max(0.0f, (max.x - style.FramePadding.x - left - size.x) * style.ButtonTextAlign.x);
        float y = min.y + style.FramePadding.y +
                  std::max(0.0f, (max.y - min.y - 2 * style.FramePadding.y - size.y) * style.ButtonTextAlign.y);
        x += ImGui::CalcTextSize(text.substr(0, at).c_str()).x;
        char glyph[2] = {text[at], '\0'};
        drawList->AddText(ImVec2(x, y), foreground(themeColor(ThemeColor::MenuShortcut)), glyph);
    }
    return pressed || hotkeyPressed(letter);
}

// The gap between two buttons: room for the first one's shadow
void nextButton()
{
    ImGui::SameLine(0.0f, 2.0f * ImGui::CalcTextSize("M").x);
}

bool lessNoCase(const std::string &a, const std::string &b)
{
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
    });
}

// '*' and '?', case-insensitive
bool matches(const char *pattern, const char *text)
{
    for (; *pattern != '\0'; pattern++, text++) {
        if (*pattern == '*') {
            for (const char *rest = text;; rest++) {
                if (matches(pattern + 1, rest)) {
                    return true;
                }
                if (*rest == '\0') {
                    return false;
                }
            }
        }
        if (*text == '\0' || (*pattern != '?' && std::tolower(static_cast<unsigned char>(*pattern)) !=
                                                     std::tolower(static_cast<unsigned char>(*text)))) {
            return false;
        }
    }
    return *text == '\0';
}

// Several patterns separated by ';' ("*.nes;*.zip")
bool matchesAny(const std::string &patterns, const char *text)
{
    size_t start = 0;
    while (start <= patterns.size()) {
        size_t end = patterns.find(';', start);
        end = end == std::string::npos ? patterns.size() : end;
        if (matches(patterns.substr(start, end - start).c_str(), text)) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

} // namespace

bool Dialog::draw()
{
    DialogColors colors;
    std::string id = title + "##dialog";
    if (firstFrame) {
        ImGui::OpenPopup(id.c_str());
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(id.c_str(), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return false;
    }
    drawShadow(
        ImGui::GetWindowDrawList(), ImGui::GetWindowPos(),
        ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y),
        ImVec2(ImGui::CalcTextSize("M").x, ImGui::GetFontSize()));
    bool open = drawBody();
    firstFrame = false;
    if (!open) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return open;
}

// --- Message box -------------------------------------------------------------------------------------------------

MessageDialog::MessageDialog(std::string title, std::string text, std::vector<std::string> buttons)
    : Dialog(std::move(title)), text(std::move(text)), buttons(std::move(buttons))
{}

bool MessageDialog::drawBody()
{
    ImGui::TextUnformatted(text.c_str());
    ImGui::Spacing();
    for (size_t i = 0; i < buttons.size(); i++) {
        if (i > 0) {
            nextButton();
        }
        if (firstFrame && i == 0) {
            ImGui::SetKeyboardFocusHere(); // (Enter presses the first button; Tab and the arrows move)
        }
        if (dialogButton(buttons[i], ImGui::GetFontSize() * 6.0f) && !firstFrame) {
            answer = static_cast<int>(i);
            return false;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        answer = -1;
        return false;
    }
    return true;
}

// --- Input box ---------------------------------------------------------------------------------------------------

InputDialog::InputDialog(std::string title, std::string label, std::string value)
    : Dialog(std::move(title)), value(std::move(value)), label(std::move(label))
{}

bool InputDialog::drawBody()
{
    ImGui::TextUnformatted(withoutTildes(label, nullptr).c_str());
    ImGui::SameLine();
    if (firstFrame) {
        ImGui::SetKeyboardFocusHere();
    }
    inputColors(true);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
    bool entered = ImGui::InputText("##value", &value, ImGuiInputTextFlags_EnterReturnsTrue);
    inputColors(false);
    ImGui::Spacing();
    const float width = ImGui::GetFontSize() * 6.0f;
    if (entered || dialogButton("OK", width)) {
        ok = true;
        return false;
    }
    nextButton();
    return !dialogButton("Cancel", width) && !ImGui::IsKeyPressed(ImGuiKey_Escape, false);
}

// --- File dialog -------------------------------------------------------------------------------------------------

FileDialog::FileDialog(std::string title, const std::string &start, std::string wildcard)
    : Dialog(std::move(title)), wildcard(std::move(wildcard)), name(this->wildcard)
{
    std::error_code ec;
    fs::path absolute = fs::absolute(start.empty() ? fs::path(".") : fs::path(start), ec);
    dir =
        (ec || !fs::is_directory(absolute, ec)) ? fs::current_path(ec).string() : absolute.lexically_normal().string();
    if (dir.size() > 1 && dir.back() == '/') {
        dir.pop_back();
    }
    readDirectory();
}

void FileDialog::readDirectory()
{
    entries.clear();
    selected = -1;
    std::error_code ec;
    if (fs::path(dir).has_parent_path() && fs::path(dir) != fs::path(dir).root_path()) {
        entries.push_back({"..", true});
    }
    std::vector<Entry> dirs, files;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::string file = it->path().filename().string();
        if (file.empty() || file[0] == '.') {
            continue; // (hidden)
        }
        std::error_code typeError;
        if (it->is_directory(typeError)) {
            dirs.push_back({file, true});
        } else if (matchesAny(wildcard, file.c_str())) {
            files.push_back({file, false});
        }
    }
    auto byName = [](const Entry &a, const Entry &b) { return lessNoCase(a.name, b.name); };
    std::sort(files.begin(), files.end(), byName);
    std::sort(dirs.begin(), dirs.end(), byName);
    entries.insert(entries.end(), files.begin(), files.end()); // (as Turbo Vision's: the files, then the directories)
    entries.insert(entries.end(), dirs.begin(), dirs.end());
}

// The first entry whose name begins with PREFIX (any case), or -1
int FileDialog::findPrefix(const std::string &prefix) const
{
    for (int i = 0; i < static_cast<int>(entries.size()); i++) {
        const std::string &n = entries[static_cast<size_t>(i)].name;
        if (n.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), n.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            })) {
            return i;
        }
    }
    return -1;
}

bool FileDialog::open(const std::string &text)
{
    if (text.empty()) {
        return false;
    }
    if (text.find_first_of("*?") != std::string::npos) {
        wildcard = text;
        readDirectory();
        return false;
    }
    std::error_code ec;
    fs::path path = fs::path(text).is_absolute() ? fs::path(text) : fs::path(dir) / text;
    path = path.lexically_normal();
    if (fs::is_directory(path, ec)) {
        dir = path.string();
        if (dir.size() > 1 && dir.back() == '/') {
            dir.pop_back();
        }
        name = wildcard;
        readDirectory();
        return false;
    }
    chosen = path.string();
    return true;
}

bool FileDialog::drawBody()
{
    const float cell = ImGui::GetFontSize();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine();
    if (firstFrame) {
        ImGui::SetKeyboardFocusHere();
    }
    inputColors(true);
    ImGui::SetNextItemWidth(cell * 30.0f);
    bool entered = ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue);
    inputColors(false);
    if (entered && open(name)) {
        return false;
    }

    // Typing while the list has the keyboard (not the name): the first entry that begins with what was typed
    // (letters typed within a second of each other add up; a letter that matches nothing on its own starts again)
    int jumpTo = -1;
    if (!firstFrame && !ImGui::IsAnyItemActive()) {
        const double now = ImGui::GetTime();
        for (ImWchar c : ImGui::GetIO().InputQueueCharacters) {
            if (c < 32 || c >= 127) {
                continue;
            }
            typed = (now - typedAt > 1.0) ? std::string() : typed;
            typedAt = now;
            typed += static_cast<char>(c);
            int found = findPrefix(typed);
            if (found < 0 && typed.size() > 1) {
                typed = std::string(1, static_cast<char>(c));
                found = findPrefix(typed);
            }
            if (found >= 0) {
                jumpTo = selected = found;
                const Entry &e = entries[static_cast<size_t>(found)];
                name = e.name + (e.directory ? "/" : "");
            }
        }
    }

    ImGui::TextUnformatted("Files");
    bool chosenNow = false;
    if (ImGui::BeginChild("##files", ImVec2(cell * 34.0f, cell * 14.0f), ImGuiChildFlags_NavFlattened)) {
        for (int i = 0; i < static_cast<int>(entries.size()); i++) {
            const Entry &e = entries[static_cast<size_t>(i)];
            std::string label = e.name + (e.directory ? "/" : "");
            if (i == jumpTo) {
                ImGui::SetKeyboardFocusHere(); // (the keyboard's cursor to it: Enter opens it)
            }
            const bool clicked = ImGui::Selectable(label.c_str(), i == selected, ImGuiSelectableFlags_AllowDoubleClick);
            if (i == jumpTo) {
                ImGui::SetScrollHereY(0.5f);
            }
            if (clicked) {
                selected = i;
                name = e.name + (e.directory ? "/" : "");
                // a double click, or Enter on it: open it (a directory: go there)
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                    ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                    chosenNow = open(e.name);
                    break;
                }
            }
        }
    }
    ImGui::EndChild();
    if (chosenNow) {
        return false;
    }
    // where it is: a long path keeps its end, as wide as the list
    std::string where = (dir == "/" ? dir : dir + "/") + wildcard;
    const size_t room = static_cast<size_t>(std::max(10.0f, cell * 34.0f / ImGui::CalcTextSize("M").x));
    if (where.size() > room) {
        where = "..." + where.substr(where.size() - (room - 3));
    }
    ImGui::TextUnformatted(where.c_str());
    ImGui::Spacing();
    const float width = cell * 6.0f;
    if (dialogButton("Open", width) && open(name)) {
        return false;
    }
    nextButton();
    if (dialogButton("Cancel", width) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        chosen.clear();
        return false;
    }
    return true;
}
