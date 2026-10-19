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
// Dialog boxes of the ImGui user interface, in Turbo Vision's gray dialog colors: a message box with buttons, a box
// asking for a line of text, and a file dialog. DebuggerApp::runDialog() shows one and waits for it to close.
#pragma once
#include <string>
#include <vector>

class Dialog {
public:
    virtual ~Dialog() = default;
    bool draw(); // once per frame; false once it has closed

protected:
    explicit Dialog(std::string title) : title(std::move(title)) {}
    virtual bool drawBody() = 0; // the inside; false: close
    std::string title;
    bool firstFrame = true;
};

// A message with buttons ("~Y~es": Y or Alt+Y presses it; Enter the first one). answer: the button's index, or -1
// for Esc.
class MessageDialog : public Dialog {
public:
    MessageDialog(std::string title, std::string text, std::vector<std::string> buttons);
    int answer = -1;

private:
    bool drawBody() override;
    std::string text;
    std::vector<std::string> buttons;
};

// A line of text. ok: OK was pressed (or Enter).
class InputDialog : public Dialog {
public:
    InputDialog(std::string title, std::string label, std::string value);
    std::string value;
    bool ok = false;

private:
    bool drawBody() override;
    std::string label;
};

// Choosing a file matching WILDCARD ("*.nes"), starting in DIR (else the current directory). Typing in the list jumps
// to the entry beginning with the letters. chosen: the file's full path, or "" (cancelled).
class FileDialog : public Dialog {
public:
    FileDialog(std::string title, const std::string &start, std::string wildcard);
    std::string chosen;

private:
    bool drawBody() override;
    void readDirectory();
    bool open(const std::string &text); // a directory: go there; a wildcard: list those; a file: choose it (true)
    int findPrefix(const std::string &prefix) const;
    struct Entry {
        std::string name;
        bool directory;
    };
    std::string dir, wildcard, name;
    std::vector<Entry> entries;
    int selected = -1;
    std::string typed;  // what was typed in the list (to jump to an entry beginning with it)
    double typedAt = 0; // when (ImGui::GetTime())
};
