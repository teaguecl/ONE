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
// The debugger's windows in the ImGui user interface, drawn as a grid of characters like the Turbo Vision ones
// (../views.hpp), with the same rows, columns and theme colors. Each view draws itself into a Canvas (the inside of
// its window) and reacts to keys, clicks and the mouse wheel; values that changed since the previous stop are
// highlighted (as in SoftICE).
#pragma once
#include <imgui.h>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include "../disasm.hpp"
#include "../theme.hpp"

// A theme color as ImGui colors ('default' = light gray on black)
inline ImU32 rgbColor(uint32_t rgb)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 0xFF);
}
inline ImU32 foreground(const ThemeAttr &a)
{
    return rgbColor(a.fgDefault ? 0xAAAAAA : a.fg);
}
inline ImU32 background(const ThemeAttr &a)
{
    return rgbColor(a.bgDefault ? 0x000000 : a.bg);
}
inline const ThemeAttr &themeColor(ThemeColor c)
{
    return Theme::instance()[c];
}

// Turbo Vision's drop shadow under a box from MIN to MAX: two columns at the right and a row below, darkening what
// is there. The menus and dialogs have it; the debugger's windows do not.
inline void drawShadow(ImDrawList *drawList, ImVec2 min, ImVec2 max, ImVec2 cell)
{
    const ImU32 shade = IM_COL32(0, 0, 0, 160);
    drawList->PushClipRectFullScreen();
    drawList->AddRectFilled(ImVec2(max.x, min.y + cell.y), ImVec2(max.x + 2 * cell.x, max.y + cell.y), shade);
    drawList->AddRectFilled(ImVec2(min.x + 2 * cell.x, max.y), ImVec2(max.x, max.y + cell.y), shade);
    drawList->PopClipRect();
}

// What a view drew (Canvas::record): shown again, unchanged, while the game runs (Canvas::replay)
struct CanvasOp {
    enum class Kind { Fill, Text } kind;
    int x, y, width; // (Fill: width; Text: limit)
    std::string text;
    ThemeAttr attr;
};
using CanvasRecording = std::vector<CanvasOp>;

// A grid of character cells on the screen: the inside of a window, the menu bar, a menu, the status line
class Canvas {
public:
    Canvas(ImDrawList *drawList, ImVec2 origin, ImVec2 cell, int cols, int rows)
        : drawList(drawList), origin(origin), cell(cell), cols(cols), rows(rows)
    {}
    void fill(int x, int y, int width, const ThemeAttr &attr); // the background of WIDTH cells
    // TEXT from cell (x, y), cut off at column LIMIT (-1: the right edge); returns the columns it took
    int text(int x, int y, const std::string &text, const ThemeAttr &attr, int limit = -1);
    void cursor(int x, int y, const ThemeAttr &attr); // a blinking underline cursor
    ImVec2 at(int x, int y) const { return ImVec2(origin.x + x * cell.x, origin.y + y * cell.y); }
    void replay(const CanvasRecording &ops); // draw a recording again (cut to this canvas's size)

    ImDrawList *drawList;
    ImVec2 origin, cell;
    int cols, rows;
    CanvasRecording *record = nullptr; // not null: fill() and text() are also added to it
};

// One row of a canvas, built from (text, color) pieces left to right
class Row {
public:
    Row(Canvas &canvas, int y) : Row(canvas, y, themeColor(ThemeColor::Text)) {}
    Row(Canvas &canvas, int y, const ThemeAttr &fill) : c(canvas), y(y) { c.fill(0, y, c.cols, fill); }
    Row &put(const std::string &text, const ThemeAttr &attr)
    {
        x += c.text(x, y, text, attr);
        return *this;
    }
    Row &skip(int n)
    {
        x += n;
        return *this;
    }

private:
    Canvas &c;
    int y;
    int x = 0;
};

// A key for a view or the command line: a key (ImGuiKey_...) with its modifiers, or a typed character (ch)
struct KeyPress {
    ImGuiKey key = ImGuiKey_None;
    char ch = 0;
    bool ctrl = false, shift = false, alt = false;
};

class View {
public:
    virtual ~View() = default;
    virtual void draw(Canvas &canvas, bool focused) = 0;
    virtual bool key(const KeyPress & /*key*/) { return false; } // true: used
    virtual void click(int /*x*/, int /*y*/, bool /*doubleClick*/) {}
    virtual void drag(int /*x*/, int /*y*/) {} // the left button held after a click (x, y may be outside the window)
    virtual void release() {}                  // ... and let go
    virtual void wheel(int /*lines*/) {}       // > 0: down
    virtual void onStop() {}                   // the emulation stopped: compare with the previous stop, then remember
    // While the game runs, a window shows what it showed when the game was last stopped (as in SoftICE), not the
    // values changing: only a view that shows no game state (the command window) is drawn live.
    virtual bool drawnWhileRunning() const { return false; }
    // Text selected with the mouse (the command and code windows): what Ctrl+C copies, lines joined by '\n'
    virtual std::string selectedText() const { return {}; }
    std::function<void(const std::string &)> onSelected; // a selection made (the X11 primary selection)
    int cols = 40, rows = 8;                             // the size of the window's inside (at the last draw)
    CanvasRecording stopped;                             // what it drew at the last frame before the game ran
};

class RegistersView : public View {
public:
    RegistersView();
    void draw(Canvas &canvas, bool focused) override;
    void onStop() override;

private:
    enum { PC, A, X, Y, SP, P, NUM };
    static unsigned current(int reg);
    unsigned previous[NUM] = {};
    bool changed[NUM] = {};
    unsigned changedFlags = 0; // P bits that changed since the last stop
};

class PpuView : public View {
public:
    PpuView();
    void draw(Canvas &canvas, bool focused) override;
    void onStop() override;

private:
    enum { CTRL, MASK, STATUS, OAMADDR, V, T, FX, W, NUM };
    static unsigned current(int reg);
    unsigned previous[NUM] = {};
    bool changed[NUM] = {};
};

struct CodeRow {
    bool isLabel = false; // a "label:" line in front of the instruction
    std::string label;
    DisasmLine line;
};

class CodeView : public View {
public:
    CodeView(Disassembler &disasm, const Symbols &symbols);
    void draw(Canvas &canvas, bool focused) override;
    bool key(const KeyPress &key) override;
    // a click moves the cursor bar there (a double click toggles a breakpoint); dragging selects text to copy
    void click(int x, int y, bool doubleClick) override;
    void drag(int x, int y) override;
    void release() override;
    std::string selectedText() const override;
    void wheel(int lines) override;
    void onStop() override;             // follow the PC
    void showAddress(uint16_t address); // 'u' command
    void reset();                       // another game: forget its scroll stops, show the PC
    uint16_t cursorAddress() const;
    std::function<void(uint16_t)> onComment;  // ';' pressed on the cursor line (its address)
    std::function<void()> onToggleBreakpoint; // a double click on a line

private:
    std::vector<CodeRow> layout() const; // the rows currently visible (labels take a row of their own)
    void scrollLines(int delta);
    void moveCursor(int delta);
    void skipLabels(int step); // the cursor never rests on a label line: on to the next instruction (STEP -1: up)
    bool mustStart(uint16_t address) const;      // an instruction starts here (PC, executed, 'u' address, scroll stop)
    DisasmLine decodeAt(uint16_t address) const; // the instruction, or a data byte if it would cover mustStart()
    Disassembler &disasm;
    const Symbols &symbols;
    uint16_t top = 0;
    std::vector<uint16_t> anchors; // 'u' address, and where scrolling up met bytes no instruction ends at
    int32_t cursorAt = -1;         // the address of the cursor line (-1: none): it stays on it when labels come or go
    int cursorRow = -1;            // -1: no cursor (shown only when the window is focused)
    // the selection, in rows and columns of what is shown (it goes when the code scrolls)
    struct Cell {
        int row = 0, col = 0;
        bool operator<(const Cell &o) const { return row < o.row || (row == o.row && col < o.col); }
    };
    std::string rowText(const CodeRow &r) const; // a row as drawn, as text
    bool selectedRange(int row, size_t length, size_t &from, size_t &to) const;
    void clearSelection() { anchor = head = Cell{}; }
    Cell anchor, head;
    bool selecting = false;
};

class MemoryView : public View {
public:
    enum class Source { Cpu, Ppu };
    explicit MemoryView(Source source);
    void draw(Canvas &canvas, bool focused) override;
    bool key(const KeyPress &key) override;
    void wheel(int lines) override;
    void onStop() override;
    void showAddress(uint16_t address, uint32_t mark = 0); // mark: highlight that many bytes from address
    std::string bankNote() const; // for the title: "   $BAB0: PRG bank 5" while the top row is banked ROM

private:
    uint8_t readByte(uint32_t address) const;
    uint32_t bytesPerRow() const; // 16, or 8 when the window is too narrow for 16
    uint32_t memorySize() const;
    void scrollBy(int32_t delta);
    Source source;
    uint32_t top = 0;
    std::vector<uint8_t> baseline; // values at the previous stop
    std::vector<uint8_t> changedFlags;
    uint32_t markStart = 0, markEnd = 0; // highlighted bytes (s match) until the next stop
};

class CommandView : public View {
public:
    explicit CommandView(std::function<void(const std::string &)> onCommand);
    void draw(Canvas &canvas, bool focused) override;
    bool drawnWhileRunning() const override { return true; } // (the debugger's own output, "Running...")
    bool key(const KeyPress &key) override;
    void wheel(int lines) override;
    // Selecting output with the mouse: a click starts it, dragging extends it (scrolling at the edges), a double
    // click takes a word; selectedText() is what Ctrl+C copies
    void click(int x, int y, bool doubleClick) override;
    void drag(int x, int y) override;
    void release() override;
    std::string selectedText() const override;
    void print(const std::string &line);
    void paste(const std::string &text); // line ends enter the line
    void setRunning(bool isRunning) { running = isRunning; }
    void setInput(const std::string &text); // put text on the command line (to be completed and entered)
    std::function<void(const std::string &)> onInputChanged;    // the command line being typed (help line)
    std::function<std::string(const std::string &)> onComplete; // Tab: the command line completed

private:
    bool editInput(const KeyPress &key);
    void enter();
    std::function<void(const std::string &)> onCommand;
    std::deque<std::string> output;
    std::vector<std::string> history;
    int historyIndex = -1;
    std::string input;
    size_t cursor = 0; // where typing goes in input (0 .. input.size())
    int scrollBack = 0;
    bool running = false;
    // the selection: from anchor to head, as (line, column); lines are counted from the first line ever printed,
    // so that they stay put when old lines are dropped (firstLine: the number of output[0])
    struct TextPos {
        long line = 0;
        int col = 0;
        bool operator<(const TextPos &o) const { return line < o.line || (line == o.line && col < o.col); }
    };
    TextPos positionAt(int x, int y) const; // the output position at a cell (clamped to the output shown)
    bool selectedRange(long line, size_t length, size_t &from, size_t &to) const; // its part of LINE
    long firstLine = 0;
    TextPos anchor, head;
    bool selecting = false; // the button is held
};

// The watch window: one row per watch (Watches), values compared with the previous stop. Keys while it has the
// focus: Up/Down select, Del removes, Enter puts "eb address " on the command line, Ins "watch ", Ctrl+F freezes.
class WatchView : public View {
public:
    explicit WatchView(const Symbols &symbols);
    void draw(Canvas &canvas, bool focused) override;
    bool key(const KeyPress &key) override;
    void click(int x, int y, bool doubleClick) override;
    void onStop() override;
    void listChanged();                                 // watches added or removed: remember the new ones' values
    std::function<void(const std::string &)> onEdit;    // text for the command line
    std::function<void(const std::string &)> onCommand; // a command to run (wc, freeze)

private:
    const Symbols &symbols;
    std::vector<std::vector<uint8_t>> baseline; // values at the previous stop, per watch
    std::vector<bool> changed;
    int selected = 0;
};

// The OAM window: the 64 sprites decoded (oam.hpp), with warnings; the visible ones only, or all (A); or OAM as
// bytes (H). Enter puts eo for the selected sprite on the command line. Values that changed since the last stop
// are highlighted.
class OamView : public View {
public:
    OamView();
    void draw(Canvas &canvas, bool focused) override;
    bool key(const KeyPress &key) override;
    void click(int x, int y, bool doubleClick) override;
    void wheel(int lines) override;
    void onStop() override;
    std::function<void(const std::string &)> onEdit; // text for the command line

private:
    std::vector<int> shownSprites() const; // the sprite numbers listed (visible ones, or all)
    int bytesPerRow() const;               // bytes mode: 16, or 8 when the window is narrow
    void moveTo(int at, const std::vector<int> &list);
    std::array<uint8_t, 256> baseline{}; // OAM at the previous stop
    std::array<bool, 256> changed{};
    bool all = false;   // all 64 sprites (else the visible ones)
    bool bytes = false; // OAM as bytes
    int selected = 0;   // a sprite number
    int top = 0;        // the first row shown
};
