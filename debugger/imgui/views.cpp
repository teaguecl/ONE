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
#include "views.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <iterator>
#include <vector>
#include <utility>
#include <functional>
#include "../core.hpp"
#include "../emulator.hpp"
#include "../oam.hpp"
#include "../watches.hpp"
#include "debugger/theme.hpp"
#include "debugger/disasm.hpp"
#include "debugger/symbols.hpp"

namespace {

std::string hex(unsigned value, int digits)
{
    char buf[12];
    std::snprintf(buf, sizeof(buf), "%0*X", digits, value);
    return buf;
}

std::string padded(std::string text, size_t width)
{
    if (text.size() < width) {
        text.append(width - text.size(), ' ');
    }
    return text;
}

} // namespace

// --- Canvas ------------------------------------------------------------------------------------------------------

void Canvas::fill(int x, int y, int width, const ThemeAttr &attr)
{
    if (record != nullptr) {
        record->push_back({CanvasOp::Kind::Fill, x, y, width, std::string(), attr});
    }
    x = std::max(0, x);
    width = std::min(width, cols - x);
    if (width <= 0 || y < 0 || y >= rows) {
        return;
    }
    ImVec2 a = at(x, y);
    drawList->AddRectFilled(a, ImVec2(a.x + width * cell.x, a.y + cell.y), background(attr));
}

int Canvas::text(int x, int y, const std::string &text, const ThemeAttr &attr, int limit)
{
    const int right = (limit < 0) ? cols : std::min(limit, cols);
    if (x >= right || y < 0 || y >= rows || text.empty()) {
        return static_cast<int>(text.size());
    }
    const int n = std::min(static_cast<int>(text.size()), right - x);
    if (record != nullptr) {
        record->push_back({CanvasOp::Kind::Text, x, y, limit, text, attr});
    }
    CanvasRecording *recording = record;
    record = nullptr; // (the background is part of the text)
    fill(x, y, n, attr);
    record = recording;
    ImVec2 a = at(x, y);
    const ImU32 color = foreground(attr);
    // (the font has room above its letters and its descenders (g, p, q, y) reach below the cell, where the next
    // row's background would cover them: draw the text a little higher)
    const float lift = std::round(cell.y * 0.1f);
    a.y -= lift;
    const char *begin = text.data(), *end = text.data() + n;
    drawList->AddText(a, color, begin, end);
    if (attr.bold) {
        drawList->AddText(ImVec2(a.x + 1.0f, a.y), color, begin, end);
    }
    if (attr.underline) {
        float base = a.y + lift + cell.y - 1.0f;
        drawList->AddLine(ImVec2(a.x, base), ImVec2(a.x + n * cell.x, base), color);
    }
    return static_cast<int>(text.size());
}

void Canvas::replay(const CanvasRecording &ops)
{
    for (const CanvasOp &op : ops) {
        if (op.kind == CanvasOp::Kind::Fill) {
            fill(op.x, op.y, op.width, op.attr);
        } else {
            text(op.x, op.y, op.text, op.attr, op.width);
        }
    }
}

void Canvas::cursor(int x, int y, const ThemeAttr &attr)
{
    if (x < 0 || x >= cols || y < 0 || y >= rows || std::fmod(ImGui::GetTime(), 1.0) >= 0.5) {
        return; // (blinks: half a second on, half off)
    }
    ImVec2 a = at(x, y);
    float height = std::max(2.0f, std::floor(cell.y / 7.0f));
    drawList->AddRectFilled(ImVec2(a.x, a.y + cell.y - height), ImVec2(a.x + cell.x, a.y + cell.y), foreground(attr));
}

// --- CPU registers -----------------------------------------------------------------------------------------------

RegistersView::RegistersView()
{
    RegistersView::onStop(); // this class's own version (constructor)
    std::fill(std::begin(changed), std::end(changed), false);
    changedFlags = 0;
}

unsigned RegistersView::current(int reg)
{
    switch (reg) {
    case PC: return cpu_debug_get_pc();
    case A: return cpu_debug_get_reg_a();
    case X: return cpu_debug_get_reg_x();
    case Y: return cpu_debug_get_reg_y();
    case SP: return 0x100 + cpu_debug_get_sp();
    case P: return cpu_debug_get_reg_p();
    }
    return 0;
}

void RegistersView::onStop()
{
    changedFlags = current(P) ^ previous[P];
    for (int i = 0; i < NUM; i++) {
        unsigned value = current(i);
        changed[i] = (value != previous[i]);
        previous[i] = value;
    }
}

void RegistersView::draw(Canvas &canvas, bool /*focused*/)
{
    // a changed register is highlighted as a whole, name and value ("PC=8261"), as in SoftICE
    auto field = [&](Row &row, const char *name, int reg, int digits) {
        const ThemeAttr &highlight = themeColor(ThemeColor::RegisterChanged);
        row.put(name, changed[reg] ? highlight : themeColor(ThemeColor::Dim))
            .put(hex(current(reg), digits), changed[reg] ? highlight : themeColor(ThemeColor::Value))
            .skip(2);
    };
    {
        Row row(canvas, 0);
        row.skip(1);
        field(row, "PC=", PC, 4);
        field(row, "A=", A, 2);
        field(row, "X=", X, 2);
        field(row, "Y=", Y, 2);
        field(row, "SP=", SP, 4);
        field(row, "P=", P, 2);
        // the flags: upper case = set; a flag that changed is highlighted on its own
        static const char flagNames[] = "NV-BDIZC";
        unsigned p = current(P);
        for (int bit = 7; bit >= 0; bit--) {
            bool set = (p >> bit) & 1;
            char name[2] = {set ? flagNames[7 - bit] : static_cast<char>(flagNames[7 - bit] | 0x20), 0};
            const ThemeAttr *color = set ? &themeColor(ThemeColor::Value) : &themeColor(ThemeColor::Dim);
            if ((changedFlags >> bit) & 1) {
                color = &themeColor(ThemeColor::RegisterChanged);
            }
            row.put(name, *color);
        }
    }
    if (canvas.rows > 1) {
        Row row(canvas, 1);
        char text[96];
        std::snprintf(text, sizeof(text), " CYC %-10llu FRAME %-7u SCANLINE %-4u DOT %u",
                      (unsigned long long)cpu_debug_get_cycle(), ppu_debug_get_frame_num(), ppu_debug_get_scanline(),
                      ppu_debug_get_scanline_clock());
        row.put(text, themeColor(ThemeColor::Text));
    }
    for (int y = 2; y < canvas.rows; y++) {
        Row row(canvas, y);
    }
}

// --- PPU registers -----------------------------------------------------------------------------------------------

PpuView::PpuView()
{
    PpuView::onStop(); // this class's own version (constructor)
    std::fill(std::begin(changed), std::end(changed), false);
}

unsigned PpuView::current(int reg)
{
    switch (reg) {
    case CTRL: return ppu_get_ppuctrl();
    case MASK: return ppu_get_ppumask();
    case STATUS: return ppu_get_ppustatus();
    case OAMADDR: return ppu_get_ppusar();
    case V: return ppu_get_vram_addr();
    case T: return ppu_debug_get_t();
    case FX: return ppu_debug_get_fine_x();
    case W: return ppu_debug_get_w() ? 1 : 0;
    }
    return 0;
}

void PpuView::onStop()
{
    for (int i = 0; i < NUM; i++) {
        unsigned value = current(i);
        changed[i] = (value != previous[i]);
        previous[i] = value;
    }
}

void PpuView::draw(Canvas &canvas, bool /*focused*/)
{
    // a changed register is highlighted as a whole, name and value, as in SoftICE
    auto field = [&](Row &row, const char *name, int reg, int digits) {
        const ThemeAttr &highlight = themeColor(ThemeColor::RegisterChanged);
        row.put(name, changed[reg] ? highlight : themeColor(ThemeColor::Dim))
            .put(hex(current(reg), digits), changed[reg] ? highlight : themeColor(ThemeColor::Value))
            .skip(2);
    };
    {
        Row row(canvas, 0);
        row.skip(1);
        field(row, "CTRL=", CTRL, 2);
        field(row, "MASK=", MASK, 2);
        field(row, "STATUS=", STATUS, 2);
    }
    if (canvas.rows > 1) {
        Row row(canvas, 1);
        row.skip(1);
        field(row, "OAMADDR=", OAMADDR, 2);
        field(row, "v=", V, 4);
        field(row, "t=", T, 4);
        field(row, "x=", FX, 1);
        field(row, "w=", W, 1);
    }
    for (int y = 2; y < canvas.rows; y++) {
        Row row(canvas, y);
    }
}

// --- Code --------------------------------------------------------------------------------------------------------

CodeView::CodeView(Disassembler &disasm, const Symbols &symbols) : disasm(disasm), symbols(symbols)
{
    top = cpu_debug_get_pc();
}

bool CodeView::mustStart(uint16_t address) const
{
    return Disassembler::knownStart(address) || std::find(anchors.begin(), anchors.end(), address) != anchors.end();
}

DisasmLine CodeView::decodeAt(uint16_t address) const
{
    if (disasm.isData(address)) {
        return disasm.dataRow(address); // bytes the code/data log saw read as data: shown as bytes
    }
    DisasmLine line = disasm.decode(address, address == cpu_debug_get_pc());
    for (int i = 1; i < line.size; i++) {
        if (mustStart(static_cast<uint16_t>(address + i)) || disasm.isData(static_cast<uint16_t>(address + i))) {
            return Disassembler::dataByte(address); // keep the instruction that starts there lined up
        }
    }
    return line;
}

std::vector<CodeRow> CodeView::layout() const
{
    std::vector<CodeRow> list;
    uint32_t address = top;
    while (static_cast<int>(list.size()) < rows && address <= 0xFFFF) {
        uint16_t a = static_cast<uint16_t>(address);
        if (const std::string *label = symbols.find(a)) {
            CodeRow labelRow;
            labelRow.isLabel = true;
            labelRow.label = *label + ":";
            labelRow.line.address = a;
            list.push_back(labelRow);
            if (static_cast<int>(list.size()) >= rows) {
                break;
            }
        }
        CodeRow row;
        row.line = decodeAt(a);
        list.push_back(row);
        address += row.line.size;
    }
    return list;
}

void CodeView::draw(Canvas &canvas, bool focused)
{
    const Emulator &emu = Emulator::instance();
    const uint16_t pc = cpu_debug_get_pc();
    std::vector<CodeRow> list = layout();
    for (size_t i = 0; cursorRow >= 0 && cursorAt >= 0 && i < list.size(); i++) {
        if (!list[i].isLabel && list[i].line.address == cursorAt) {
            cursorRow = static_cast<int>(i); // (a label put in front of the cursor line moved it down a row)
        }
    }
    for (int y = 0; y < canvas.rows; y++) {
        if (y >= static_cast<int>(list.size())) {
            Row row(canvas, y);
            continue;
        }
        const CodeRow &r = list[static_cast<size_t>(y)];
        if (r.isLabel) {
            Row row(canvas, y);
            row.put(r.label, themeColor(ThemeColor::Label));
            continue;
        }
        const DisasmLine &line = r.line;
        bool isPc = (line.address == pc);
        bool isBp = emu.breakpointAt(line.address) >= 0;
        bool isCursor = focused && y == cursorRow && !isPc;
        bool plain = !isPc && !isBp && !isCursor; // plain lines get per-column colors
        const ThemeAttr &fill =
            themeColor(isPc       ? (isBp ? ThemeColor::CurrentBreakpointLine : ThemeColor::CurrentLine)
                       : isCursor ? (isBp ? ThemeColor::CursorBreakpointLine : ThemeColor::CursorLine)
                       : isBp     ? ThemeColor::BreakpointLine
                                  : ThemeColor::Text);
        Row row(canvas, y, fill);
        std::string mnemonic = line.mnemonic;
        if (!line.documented) {
            for (char &c : mnemonic) {
                c = static_cast<char>(c | 0x20); // unofficial opcodes in lower case
            }
        }
        row.put(isPc ? ">" : " ", fill)
            .put(Disassembler::addressColumn(line.address), plain ? themeColor(ThemeColor::Dim) : fill)
            .put("  " + padded(line.bytes, 10), plain ? themeColor(ThemeColor::Dim) : fill)
            .put(padded(mnemonic, 5), plain ? themeColor(ThemeColor::Value) : fill)
            .put(padded(line.operand, 18), fill);
        if (!line.comment.empty()) {
            row.put("; " + line.comment, plain ? themeColor(ThemeColor::Label) : fill);
        }
        if (const std::string *comment = symbols.commentAt(line.address)) {
            // the user's comment, on this one line: line breaks become spaces, the window's edge cuts it off
            std::string text = *comment;
            std::replace(text.begin(), text.end(), '\n', ' ');
            row.put((line.comment.empty() ? "; " : "  ; ") + text, plain ? themeColor(ThemeColor::Label) : fill);
        }
    }
    // the selection, over it all, in the cursor line's color
    const int lastRow = std::max(anchor, head).row;
    for (int y = 0; y < canvas.rows && y < static_cast<int>(list.size()); y++) {
        std::string text = rowText(list[static_cast<size_t>(y)]);
        size_t from = 0, to = 0;
        if (selectedRange(y, text.size(), from, to)) {
            canvas.text(static_cast<int>(from), y, text.substr(from, to - from) + (y < lastRow ? " " : ""),
                        themeColor(ThemeColor::CursorLine));
        }
    }
}

std::string CodeView::rowText(const CodeRow &r) const
{
    if (r.isLabel) {
        return r.label;
    }
    const DisasmLine &line = r.line;
    std::string mnemonic = line.mnemonic;
    if (!line.documented) {
        for (char &c : mnemonic) {
            c = static_cast<char>(c | 0x20);
        }
    }
    std::string text = (line.address == cpu_debug_get_pc() ? ">" : " ") + Disassembler::addressColumn(line.address) +
                       "  " + padded(line.bytes, 10) + padded(mnemonic, 5) + padded(line.operand, 18);
    if (!line.comment.empty()) {
        text += "; " + line.comment;
    }
    if (const std::string *comment = symbols.commentAt(line.address)) {
        std::string userComment = *comment;
        std::replace(userComment.begin(), userComment.end(), '\n', ' ');
        text += (line.comment.empty() ? "; " : "  ; ") + userComment;
    }
    return text;
}

bool CodeView::selectedRange(int row, size_t length, size_t &from, size_t &to) const
{
    Cell a = std::min(anchor, head), b = std::max(anchor, head);
    if (!(a < b) || row < a.row || row > b.row) {
        return false;
    }
    from = (row == a.row) ? std::min(length, static_cast<size_t>(a.col)) : 0;
    to = (row == b.row) ? std::min(length, static_cast<size_t>(b.col)) : length;
    return true;
}

std::string CodeView::selectedText() const
{
    std::vector<CodeRow> list = layout();
    std::string text;
    const Cell a = std::min(anchor, head);
    for (int y = 0; y < static_cast<int>(list.size()); y++) {
        std::string row = rowText(list[static_cast<size_t>(y)]);
        size_t from = 0, to = 0;
        if (selectedRange(y, row.size(), from, to)) {
            std::string part = row.substr(from, to - from);
            part.erase(part.find_last_not_of(' ') + 1); // (the columns' padding at the end of a line)
            text += (y > a.row ? "\n" : "") + part;
        }
    }
    return text;
}

void CodeView::onStop()
{
    clearSelection();
    uint16_t pc = cpu_debug_get_pc();
    std::vector<CodeRow> list = layout();
    int pcRow = -1;
    for (size_t i = 0; i < list.size(); i++) {
        if (!list[i].isLabel && list[i].line.address == pc) {
            pcRow = static_cast<int>(i);
        }
    }
    if (pcRow < 0 || pcRow > rows - 3) {
        anchors.clear();
        top = disasm.backtrack(pc, std::max(1, std::min(4, rows / 4)));
        list = layout();
        pcRow = 0;
        for (size_t i = 0; i < list.size(); i++) {
            if (!list[i].isLabel && list[i].line.address == pc) {
                pcRow = static_cast<int>(i);
            }
        }
    }
    cursorRow = pcRow;
    skipLabels(1);
}

void CodeView::reset()
{
    clearSelection();
    anchors.clear();
    cursorRow = -1;
    cursorAt = -1;
    top = disasm.backtrack(cpu_debug_get_pc(), std::max(1, std::min(4, rows / 4)));
}

void CodeView::showAddress(uint16_t address)
{
    clearSelection();
    anchors.assign(1, address); // shown as an instruction even inside what looks like another one
    top = address;
    cursorRow = 0;
    skipLabels(1);
}

uint16_t CodeView::cursorAddress() const
{
    std::vector<CodeRow> list = layout();
    if (cursorRow >= 0 && cursorRow < static_cast<int>(list.size())) {
        return list[static_cast<size_t>(cursorRow)].line.address;
    }
    return cpu_debug_get_pc();
}

void CodeView::scrollLines(int delta)
{
    clearSelection();
    while (delta > 0) {
        top = static_cast<uint16_t>(top + decodeAt(top).size);
        delta--;
    }
    while (delta < 0) {
        uint16_t before = disasm.isData(static_cast<uint16_t>(top - 1))
                              ? disasm.dataRowStart(static_cast<uint16_t>(top - 1))
                              : disasm.previous(top);
        if (static_cast<uint16_t>(before + decodeAt(before).size) != top) {
            anchors.push_back(top); // no instruction ends here: keep the rows below as they were
            if (anchors.size() > 64) {
                anchors.erase(anchors.begin());
            }
        }
        top = before;
        delta++;
    }
}

void CodeView::skipLabels(int step)
{
    for (int guard = 0; guard < 2 * rows; guard++) {
        std::vector<CodeRow> list = layout();
        bool inList = cursorRow >= 0 && cursorRow < static_cast<int>(list.size());
        if (!inList || !list[static_cast<size_t>(cursorRow)].isLabel) {
            cursorAt = inList ? list[static_cast<size_t>(cursorRow)].line.address : -1;
            return;
        }
        if (step < 0 && cursorRow == 0) {
            scrollLines(-1); // (more rows above)
            list = layout();
            if (!list.empty() && list[0].isLabel) {
                step = 1; // the window starts with a label: the instruction below it
            }
            continue;
        }
        cursorRow += step;
        if (cursorRow >= rows) {
            scrollLines(1);
            cursorRow = rows - 1;
        }
    }
}

void CodeView::moveCursor(int delta)
{
    cursorRow += delta;
    if (cursorRow < 0) {
        scrollLines(cursorRow);
        cursorRow = 0;
    } else if (cursorRow >= rows) {
        scrollLines(cursorRow - rows + 1);
        cursorRow = rows - 1;
    }
    skipLabels(delta < 0 ? -1 : 1);
}

bool CodeView::key(const KeyPress &key)
{
    if (key.ch == ';') {
        if (onComment) {
            onComment(cursorAddress()); // comment the cursor line (IDA / Ghidra style)
        }
        return true;
    }
    if (key.ctrl || key.alt) {
        return false;
    }
    switch (key.key) {
    case ImGuiKey_UpArrow: moveCursor(-1); return true;
    case ImGuiKey_DownArrow: moveCursor(1); return true;
    case ImGuiKey_PageUp: scrollLines(-(rows - 1)); return true;
    case ImGuiKey_PageDown: scrollLines(rows - 1); return true;
    case ImGuiKey_Home: onStop(); return true; // back to the current instruction
    default: return false;
    }
}

void CodeView::click(int x, int y, bool doubleClick)
{
    cursorRow = y;
    skipLabels(1);
    anchor = head = Cell{y, x};
    selecting = !doubleClick;
    if (doubleClick && onToggleBreakpoint) {
        onToggleBreakpoint();
    }
}

void CodeView::drag(int x, int y)
{
    if (selecting) {
        head = Cell{std::clamp(y, 0, rows - 1), std::max(0, x)};
    }
}

void CodeView::release()
{
    if (selecting && onSelected && (anchor < head || head < anchor)) {
        onSelected(selectedText());
    }
    selecting = false;
}

void CodeView::wheel(int lines)
{
    scrollLines(lines);
}

// --- Watches -----------------------------------------------------------------------------------------------------

WatchView::WatchView(const Symbols &symbols) : symbols(symbols)
{
    listChanged();
}

void WatchView::listChanged()
{
    const std::vector<Watch> &list = Watches::instance().list();
    baseline.clear();
    for (const Watch &w : list) {
        baseline.push_back(w.read());
    }
    changed.assign(list.size(), false);
    selected = std::max(0, std::min(selected, static_cast<int>(list.size()) - 1));
}

void WatchView::onStop()
{
    const std::vector<Watch> &list = Watches::instance().list();
    baseline.resize(list.size());
    changed.assign(list.size(), false);
    for (size_t i = 0; i < list.size(); i++) {
        std::vector<uint8_t> now = list[i].read();
        changed[i] = !baseline[i].empty() && now != baseline[i];
        baseline[i] = std::move(now);
    }
}

void WatchView::draw(Canvas &canvas, bool focused)
{
    const std::vector<Watch> &list = Watches::instance().list();
    const Emulator &emu = Emulator::instance();
    if (changed.size() < list.size()) {
        changed.resize(list.size(), false); // (a watch added before listChanged())
    }
    for (int y = 0; y < canvas.rows; y++) {
        const auto i = static_cast<size_t>(y);
        if (i >= list.size()) {
            Row row(canvas, y);
            if (list.empty() && y == 0) {
                row.put("No watches: watch address [type] [name]", themeColor(ThemeColor::Dim));
            }
            continue;
        }
        const Watch &w = list[i];
        bool frozen = false;
        for (const Emulator::Freeze &f : emu.frozen()) {
            frozen = frozen || (!w.ppu && f.address >= w.address && f.address < w.address + w.length);
        }
        const bool cursor = focused && y == selected;
        const ThemeAttr &fill = themeColor(cursor ? ThemeColor::CursorLine : ThemeColor::Text);
        Row row(canvas, y, fill);
        row.put(padded(std::to_string(i), 3), cursor ? fill : themeColor(ThemeColor::Dim))
            .put(padded(w.label(symbols), 13), cursor ? fill : themeColor(ThemeColor::Label))
            .put(padded(w.addressText(), 7), fill)
            .put(padded(w.typeText(), 5), cursor ? fill : themeColor(ThemeColor::Dim))
            .put(frozen ? "F " : "  ", cursor ? fill : themeColor(ThemeColor::Value))
            .put(w.valueText(symbols),
                 cursor ? fill : themeColor(changed[i] ? ThemeColor::Changed : ThemeColor::Value));
    }
}

bool WatchView::key(const KeyPress &key)
{
    const std::vector<Watch> &list = Watches::instance().list();
    const int count = static_cast<int>(list.size());
    const bool have = selected >= 0 && selected < count;
    if (key.ctrl && key.key == ImGuiKey_F) {
        if (have && !list[static_cast<size_t>(selected)].ppu && onCommand) {
            const Watch &w = list[static_cast<size_t>(selected)];
            bool frozen = false;
            for (const Emulator::Freeze &f : Emulator::instance().frozen()) {
                frozen = frozen || f.address == w.address;
            }
            for (int b = 0; b < w.length; b++) {
                onCommand((frozen ? "unfreeze " : "freeze ") + hex(static_cast<unsigned>(w.address + b), 4));
            }
        }
        return true;
    }
    if (key.ctrl || key.alt) {
        return false;
    }
    switch (key.key) {
    case ImGuiKey_UpArrow: selected = std::max(0, selected - 1); return true;
    case ImGuiKey_DownArrow: selected = std::max(0, std::min(count - 1, selected + 1)); return true;
    case ImGuiKey_Home: selected = 0; return true;
    case ImGuiKey_End: selected = std::max(0, count - 1); return true;
    case ImGuiKey_Delete:
        if (have && onCommand) {
            onCommand("wc " + std::to_string(selected));
        }
        return true;
    case ImGuiKey_Enter:
    case ImGuiKey_KeypadEnter:
        if (have && !list[static_cast<size_t>(selected)].ppu && onEdit) {
            onEdit("eb " + hex(list[static_cast<size_t>(selected)].address, 4) + " ");
        }
        return true;
    case ImGuiKey_Insert:
        if (onEdit) {
            onEdit("watch ");
        }
        return true;
    default: return false;
    }
}

void WatchView::click(int /*x*/, int y, bool /*doubleClick*/)
{
    selected = std::min(y, std::max(0, static_cast<int>(Watches::instance().list().size()) - 1));
}

// --- OAM ---------------------------------------------------------------------------------------------------------

OamView::OamView()
{
    for (int a = 0; a < 256; a++) {
        baseline[static_cast<size_t>(a)] = ppu_debug_read_oam(static_cast<uint8_t>(a));
    }
}

void OamView::onStop()
{
    for (int a = 0; a < 256; a++) {
        uint8_t value = ppu_debug_read_oam(static_cast<uint8_t>(a));
        changed[static_cast<size_t>(a)] = value != baseline[static_cast<size_t>(a)];
        baseline[static_cast<size_t>(a)] = value;
    }
}

int OamView::bytesPerRow() const
{
    return cols >= 3 + 16 * 3 + 3 ? 16 : 8;
}

std::vector<int> OamView::shownSprites() const
{
    std::vector<int> list;
    for (int i = 0; i < 64; i++) {
        if (all || ppu_debug_read_oam(static_cast<uint8_t>(i * 4)) < 0xEF) {
            list.push_back(i);
        }
    }
    return list;
}

void OamView::draw(Canvas &canvas, bool focused)
{
    if (bytes) { // OAM as bytes: 16 a row (8 in a narrow window), a sprite every 4
        const int perRow = bytesPerRow();
        for (int y = 0; y < canvas.rows; y++) {
            Row row(canvas, y);
            int base = (top + y) * perRow;
            if (base < 256) {
                row.put(hex(static_cast<unsigned>(base), 2) + " ", themeColor(ThemeColor::Dim));
                for (int i = 0; i < perRow; i++) {
                    auto a = static_cast<size_t>(base) + static_cast<size_t>(i);
                    row.put(hex(ppu_debug_read_oam(static_cast<uint8_t>(a)), 2) + (i % 4 == 3 ? "  " : " "),
                            themeColor(changed[a] ? ThemeColor::Changed : ThemeColor::Value));
                }
            }
        }
        return;
    }
    const OamReport report = decodeOam();
    const std::vector<std::string> header = oamHeader(report);
    const std::vector<int> list = shownSprites();
    const int headerRows = 3;
    for (int y = 0; y < canvas.rows; y++) {
        if (y < headerRows) {
            Row row(canvas, y);
            if (y < 2) {
                bool warn = y == 1 && (!report.overflows.empty() || !report.spritesOn);
                row.put(header[static_cast<size_t>(y)], themeColor(warn ? ThemeColor::Changed : ThemeColor::Label));
            } else {
                row.put(oamColumnTitles(), themeColor(ThemeColor::Dim));
            }
            continue;
        }
        const int i = top + y - headerRows;
        if (i >= static_cast<int>(list.size())) {
            Row row(canvas, y);
            if (list.empty() && y == headerRows) {
                row.put("No visible sprites (A: show all 64)", themeColor(ThemeColor::Dim));
            }
            continue;
        }
        const int n = list[static_cast<size_t>(i)];
        const bool cursor = focused && n == selected;
        bool rowChanged = false;
        for (int k = 0; k < 4; k++) {
            rowChanged = rowChanged || changed[(static_cast<size_t>(n) * 4) + static_cast<size_t>(k)];
        }
        const OamSprite &sprite = report.sprites[static_cast<size_t>(n)];
        const bool trouble = sprite.droppedFirst >= 0 || sprite.leftMasked;
        const ThemeAttr &color = themeColor(cursor          ? ThemeColor::CursorLine
                                            : rowChanged    ? ThemeColor::Changed
                                            : sprite.hidden ? ThemeColor::Dim
                                                            : ThemeColor::Value);
        Row row(canvas, y, color);
        std::string text = oamRow(report, n);
        std::string notes = oamNotes(report, n);
        row.put(text.substr(0, text.size() - notes.size()), color);
        row.put(notes, cursor ? color : themeColor(trouble ? ThemeColor::Changed : ThemeColor::Label));
    }
}

// Select the AT-th sprite listed (decoded), or show from row AT (bytes); keeps the selection in view
void OamView::moveTo(int at, const std::vector<int> &list)
{
    const int shown = std::max(1, rows - (bytes ? 0 : 3));
    const int count = bytes ? 256 / bytesPerRow() : static_cast<int>(list.size());
    if (bytes) {
        top = std::clamp(at, 0, std::max(0, count - shown));
    } else if (!list.empty()) {
        at = std::clamp(at, 0, static_cast<int>(list.size()) - 1);
        selected = list[static_cast<size_t>(at)];
        if (at < top) {
            top = at;
        } else if (at >= top + shown) {
            top = at - shown + 1;
        }
    }
}

bool OamView::key(const KeyPress &key)
{
    if (key.ctrl || key.alt) {
        return false;
    }
    std::vector<int> list = shownSprites();
    const int shown = std::max(1, rows - (bytes ? 0 : 3));
    const int count = bytes ? 256 / bytesPerRow() : static_cast<int>(list.size());
    auto position = std::find(list.begin(), list.end(), selected);
    int at = bytes ? top : (position == list.end()) ? 0 : static_cast<int>(position - list.begin());
    if (key.ch != 0) {
        if (key.ch == 'a' || key.ch == 'A') {
            all = !all;
            list = shownSprites();
            top = 0;
            at = std::max(0, static_cast<int>(std::find(list.begin(), list.end(), selected) - list.begin()));
            if (at >= static_cast<int>(list.size())) {
                at = 0;
            }
        } else if (key.ch == 'h' || key.ch == 'H') {
            bytes = !bytes;
            top = 0;
            at = bytes ? 0 : at;
        } else {
            return false;
        }
        moveTo(at, list);
        return true;
    }
    switch (key.key) {
    case ImGuiKey_UpArrow: at--; break;
    case ImGuiKey_DownArrow: at++; break;
    case ImGuiKey_PageUp: at -= shown; break;
    case ImGuiKey_PageDown: at += shown; break;
    case ImGuiKey_Home: at = 0; break;
    case ImGuiKey_End: at = count - 1; break;
    case ImGuiKey_Enter:
    case ImGuiKey_KeypadEnter:
        if (onEdit && !bytes) {
            onEdit("eo " + hex(static_cast<unsigned>(selected * 4), 2) + " ");
        }
        return true;
    default: return false;
    }
    moveTo(at, list);
    return true;
}

void OamView::click(int /*x*/, int y, bool /*doubleClick*/)
{
    if (bytes) {
        return;
    }
    std::vector<int> list = shownSprites();
    int i = top + y - 3;
    if (i >= 0 && i < static_cast<int>(list.size())) {
        selected = list[static_cast<size_t>(i)];
    }
}

void OamView::wheel(int lines)
{
    const int shown = std::max(1, rows - (bytes ? 0 : 3));
    const int count = bytes ? 256 / bytesPerRow() : static_cast<int>(shownSprites().size());
    top = std::clamp(top + lines, 0, std::max(0, count - shown));
}

// --- Memory ------------------------------------------------------------------------------------------------------

MemoryView::MemoryView(Source source) : source(source)
{
    baseline.resize(memorySize());
    changedFlags.assign(memorySize(), 0);
    for (uint32_t a = 0; a < memorySize(); a++) {
        baseline[a] = readByte(a);
    }
}

uint32_t MemoryView::memorySize() const
{
    return (source == Source::Cpu) ? 0x10000 : 0x4000;
}

uint8_t MemoryView::readByte(uint32_t address) const
{
    return (source == Source::Cpu) ? cpu_debug_read(static_cast<uint16_t>(address))
                                   : ppu_debug_read(static_cast<uint16_t>(address));
}

void MemoryView::onStop()
{
    for (uint32_t a = 0; a < memorySize(); a++) {
        uint8_t value = readByte(a);
        changedFlags[a] = (value != baseline[a]);
        baseline[a] = value;
    }
    markStart = markEnd = 0;
}

uint32_t MemoryView::bytesPerRow() const
{
    return (cols >= 4 + 1 + 16 * 3) ? 16 : 8; // "addr  xx xx ..." must fit
}

void MemoryView::showAddress(uint16_t address, uint32_t mark)
{
    markStart = address;
    markEnd = address + mark;
    top = (address & ~(bytesPerRow() - 1)) % memorySize();
}

std::string MemoryView::bankNote() const
{
    return (source == Source::Cpu && top <= 0xFFFF) ? Disassembler::memoryBankNote(static_cast<uint16_t>(top)) : "";
}

void MemoryView::draw(Canvas &canvas, bool /*focused*/)
{
    const uint32_t perRow = bytesPerRow();
    top &= ~(perRow - 1);
    for (int y = 0; y < canvas.rows; y++) {
        Row row(canvas, y);
        uint32_t address = top + static_cast<uint32_t>(y) * perRow;
        if (address < memorySize()) {
            row.put(hex(address, 4), themeColor(ThemeColor::Dim)).skip(1);
            for (uint32_t i = 0; i < perRow && address + i < memorySize(); i++) {
                row.put(i == perRow / 2 ? "-" : " ", themeColor(ThemeColor::Dim));
                uint8_t value = readByte(address + i);
                const uint32_t a = address + i;
                const bool marked = (a >= markStart && a < markEnd);
                row.put(hex(value, 2), marked            ? themeColor(ThemeColor::CursorLine)
                                       : changedFlags[a] ? themeColor(ThemeColor::Changed)
                                                         : themeColor(ThemeColor::Text));
            }
        }
    }
}

void MemoryView::scrollBy(int32_t delta)
{
    const int32_t perRow = static_cast<int32_t>(bytesPerRow());
    int32_t newTop = static_cast<int32_t>(top) + delta;
    int32_t last = std::max<int32_t>(0, static_cast<int32_t>(memorySize()) - perRow * std::max(1, rows));
    top = static_cast<uint32_t>(std::clamp(newTop, 0, last));
}

bool MemoryView::key(const KeyPress &key)
{
    if (key.ctrl || key.alt) {
        return false;
    }
    const int32_t perRow = static_cast<int32_t>(bytesPerRow());
    const int32_t page = perRow * std::max(1, rows);
    switch (key.key) {
    case ImGuiKey_UpArrow: scrollBy(-perRow); return true;
    case ImGuiKey_DownArrow: scrollBy(perRow); return true;
    case ImGuiKey_PageUp: scrollBy(-page); return true;
    case ImGuiKey_PageDown: scrollBy(page); return true;
    case ImGuiKey_Home: top = 0; return true;
    default: return false;
    }
}

void MemoryView::wheel(int lines)
{
    scrollBy(lines * static_cast<int32_t>(bytesPerRow()));
}

// --- Command line ------------------------------------------------------------------------------------------------

CommandView::CommandView(std::function<void(const std::string &)> onCommand) : onCommand(std::move(onCommand)) {}

void CommandView::print(const std::string &line)
{
    output.push_back(line);
    while (output.size() > 1000) {
        output.pop_front();
        firstLine++;
    }
    scrollBack = 0;
}

CommandView::TextPos CommandView::positionAt(int x, int y) const
{
    const int outputRows = std::max(1, rows - 1);
    const int last = static_cast<int>(output.size()) - scrollBack; // one past the last line shown
    int index = std::clamp(last - outputRows + std::clamp(y, 0, outputRows - 1), 0,
                           std::max(0, static_cast<int>(output.size()) - 1));
    return TextPos{firstLine + index, std::max(0, x)};
}

bool CommandView::selectedRange(long line, size_t length, size_t &from, size_t &to) const
{
    TextPos a = std::min(anchor, head), b = std::max(anchor, head);
    if (!(a < b) || line < a.line || line > b.line) {
        return false;
    }
    from = (line == a.line) ? std::min(length, static_cast<size_t>(a.col)) : 0;
    to = (line == b.line) ? std::min(length, static_cast<size_t>(b.col)) : length;
    return true;
}

std::string CommandView::selectedText() const
{
    std::string text;
    TextPos a = std::min(anchor, head), b = std::max(anchor, head);
    for (long line = std::max(a.line, firstLine); line <= b.line; line++) {
        const auto index = static_cast<size_t>(line - firstLine);
        if (index >= output.size()) {
            break;
        }
        size_t from = 0, to = 0;
        if (selectedRange(line, output[index].size(), from, to)) {
            if (line > a.line) {
                text += '\n';
            }
            text += output[index].substr(from, to - from);
        }
    }
    return text;
}

void CommandView::click(int x, int y, bool doubleClick)
{
    if (y >= rows - 1 || output.empty()) { // (the command line)
        anchor = head = TextPos{};
        selecting = false;
        return;
    }
    anchor = head = positionAt(x, y);
    selecting = true;
    if (doubleClick) { // the word under the mouse
        const std::string &text = output[static_cast<size_t>(anchor.line - firstLine)];
        auto inWord = [&](int i) {
            return i >= 0 && i < static_cast<int>(text.size()) && text[static_cast<size_t>(i)] != ' ';
        };
        int start = anchor.col, end = anchor.col;
        while (inWord(start - 1)) {
            start--;
        }
        while (inWord(end)) {
            end++;
        }
        anchor.col = start;
        head.col = end;
        selecting = false;
        if (onSelected && start < end) {
            onSelected(selectedText());
        }
    }
}

void CommandView::drag(int x, int y)
{
    if (!selecting) {
        return;
    }
    // past the top or the bottom of the output: scroll a line (each frame) and select on
    const int maxBack = std::max(0, static_cast<int>(output.size()) - (rows - 1));
    if (y < 0) {
        scrollBack = std::min(scrollBack + 1, maxBack);
    } else if (y >= rows - 1) {
        scrollBack = std::max(0, scrollBack - 1);
    }
    head = positionAt(x, y);
}

void CommandView::release()
{
    if (selecting && onSelected && (anchor.line != head.line || anchor.col != head.col)) {
        onSelected(selectedText());
    }
    selecting = false;
}

void CommandView::setInput(const std::string &text)
{
    input = text;
    cursor = input.size();
    historyIndex = -1;
    if (onInputChanged) {
        onInputChanged(input);
    }
}

void CommandView::draw(Canvas &canvas, bool /*focused*/)
{
    const int outputRows = canvas.rows - 1;
    const int last = static_cast<int>(output.size()) - scrollBack; // one past the last line shown
    for (int y = 0; y < outputRows; y++) {
        Row row(canvas, y);
        int index = last - outputRows + y;
        if (index >= 0 && index < static_cast<int>(output.size())) {
            const std::string &line = output[static_cast<size_t>(index)];
            row.put(line,
                    (!line.empty() && line[0] == ':') ? themeColor(ThemeColor::Value) : themeColor(ThemeColor::Text));
            size_t from = 0, to = 0;
            if (selectedRange(firstLine + index, line.size(), from, to)) { // the selection, in the cursor line's color
                const TextPos b = std::max(anchor, head);
                std::string part = line.substr(from, to - from);
                if (firstLine + index < b.line) {
                    part += ' '; // (the line break is part of it)
                }
                canvas.text(static_cast<int>(from), y, part, themeColor(ThemeColor::CursorLine));
            }
        }
    }
    const int y = canvas.rows - 1;
    Row prompt(canvas, y);
    if (running) {
        prompt.put("Running... Ctrl+D or F5 to stop", themeColor(ThemeColor::Running));
    } else {
        std::string text = ":" + input;
        const int column = 1 + static_cast<int>(std::min(cursor, input.size())); // the cursor, in text
        int offset = std::max(0, column - (canvas.cols - 1)); // scrolled so that the cursor stays visible
        prompt.put(text.substr(static_cast<size_t>(offset)), themeColor(ThemeColor::Value));
        canvas.cursor(column - offset, y, themeColor(ThemeColor::Value));
    }
}

void CommandView::enter()
{
    std::string line = input;
    input.clear();
    cursor = 0;
    historyIndex = -1;
    if (!line.empty() && (history.empty() || history.back() != line)) {
        history.push_back(line);
    }
    onCommand(line);
}

void CommandView::paste(const std::string &text)
{
    const std::string before = input;
    for (char c : text) {
        if (c == '\n') {
            enter();
        } else if (c >= 32 && c < 127) {
            input.insert(cursor++, 1, c);
        }
    }
    if (input != before && onInputChanged) {
        onInputChanged(input);
    }
}

bool CommandView::key(const KeyPress &key)
{
    const std::string before = input;
    bool handled = editInput(key);
    if (input != before && onInputChanged) {
        onInputChanged(input);
    }
    return handled;
}

bool CommandView::editInput(const KeyPress &key)
{
    if (key.ch != 0) {
        if (key.ch >= 32 && key.ch < 127) {
            input.insert(cursor++, 1, key.ch);
            return true;
        }
        return false;
    }
    if (key.alt) {
        return false;
    }
    if (key.ctrl) {                          // Ctrl+Left / Ctrl+Right: by words
        if (key.key == ImGuiKey_LeftArrow) { // to the start of this (or the previous) word
            while (cursor > 0 && input[cursor - 1] == ' ') {
                cursor--;
            }
            while (cursor > 0 && input[cursor - 1] != ' ') {
                cursor--;
            }
            return true;
        }
        if (key.key == ImGuiKey_RightArrow) { // to the start of the next word
            while (cursor < input.size() && input[cursor] != ' ') {
                cursor++;
            }
            while (cursor < input.size() && input[cursor] == ' ') {
                cursor++;
            }
            return true;
        }
        return false;
    }
    switch (key.key) {
    case ImGuiKey_Enter:
    case ImGuiKey_KeypadEnter: enter(); return true;
    case ImGuiKey_Tab:
        if (onComplete) {
            input = onComplete(input);
            cursor = input.size();
            historyIndex = -1;
        }
        return true;
    case ImGuiKey_Backspace:
        if (cursor > 0) {
            input.erase(--cursor, 1);
        }
        return true;
    case ImGuiKey_Delete:
        if (cursor < input.size()) {
            input.erase(cursor, 1);
        }
        return true;
    case ImGuiKey_LeftArrow:
        if (cursor > 0) {
            cursor--;
        }
        return true;
    case ImGuiKey_RightArrow:
        if (cursor < input.size()) {
            cursor++;
        }
        return true;
    case ImGuiKey_Home: cursor = 0; return true;
    case ImGuiKey_End: cursor = input.size(); return true;
    case ImGuiKey_Escape:
        input.clear();
        cursor = 0;
        return true;
    case ImGuiKey_UpArrow:
    case ImGuiKey_DownArrow:
        if (history.empty()) {
            return true;
        }
        if (key.key == ImGuiKey_UpArrow) {
            historyIndex = (historyIndex < 0) ? static_cast<int>(history.size()) - 1 : std::max(0, historyIndex - 1);
        } else {
            historyIndex = (historyIndex < 0) ? -1 : historyIndex + 1;
            if (historyIndex >= static_cast<int>(history.size())) {
                historyIndex = -1;
            }
        }
        input = (historyIndex < 0) ? std::string() : history[static_cast<size_t>(historyIndex)];
        cursor = input.size();
        return true;
    case ImGuiKey_PageUp:
        scrollBack = std::min(scrollBack + (rows - 2), std::max(0, static_cast<int>(output.size()) - (rows - 1)));
        return true;
    case ImGuiKey_PageDown: scrollBack = std::max(0, scrollBack - (rows - 2)); return true;
    default: return false;
    }
}

void CommandView::wheel(int lines)
{
    scrollBack = std::clamp(scrollBack - lines, 0, std::max(0, static_cast<int>(output.size()) - (rows - 1)));
}
