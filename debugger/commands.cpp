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
#include "commands.hpp"
#include "bg_viewer.hpp"
#include "debugger/emulator.hpp"
#include "debugger/profiler.hpp"
#include "debugger/search.hpp"
#include "debugger/flow.hpp"
#include "debugger/cdl.hpp"
#include "palette_viewer.hpp"
#include "profiler_window.hpp"
#include "pattern_viewer.hpp"
#include "disasm.hpp"
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <optional>
#include <ios>
#include <sstream>
#include <vector>
#include <string>
#include <system_error>
#include "core.hpp"
extern "C" {
#include "zipfile.h"
}
#include "frontend.h"
#include "gamegenie.hpp"
#include "oam.hpp"
#include "watches.hpp"
#include "remote.hpp"

namespace {

std::vector<std::string> split(const std::string &line)
{
    // words separated by blanks; '=' and ',' are separators too ("r a = 12", "eb 300 = 1,2,3")
    std::vector<std::string> words;
    std::string word;
    for (char c : line) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '=' || c == ',') {
            if (!word.empty()) {
                words.push_back(word);
                word.clear();
            }
        } else {
            word += c;
        }
    }
    if (!word.empty()) {
        words.push_back(word);
    }
    return words;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "$1F" / "1F" -> hex
std::optional<unsigned long> parseHex(const std::string &text)
{
    std::string digits = (!text.empty() && text[0] == '$') ? text.substr(1) : text;
    if (digits.empty()) {
        return std::nullopt;
    }
    char *end = nullptr;
    unsigned long value = std::strtoul(digits.c_str(), &end, 16);
    if (*end != '\0') {
        return std::nullopt;
    }
    return value;
}

// decimal, or hex with a leading '$'
// Words that may follow a command (only cs has them), for the help line: name, syntax, what it does.
struct SubCommand {
    const char *name;
    const char *syntax;
    const char *what;
};
// (not reformatted: a table)
// clang-format off
const SubCommand kSearchSubCommands[] = {
    {"start",      "cs start [cpu|ppu|oam]", "Start a search: every byte of that memory is a candidate (default cpu)"},
    {"stop",       "cs stop",                "End the search"},
    {"reset",      "cs reset",               "Start over: every byte of the same memory is a candidate again"},
    {"list",       "cs list [first]",        "List the candidates, 16 at a time (first: decimal, or $hex)"},
    {"undo",       "cs undo",                "Take back the last filter"},
    {"signed",     "cs signed",              "Compare values as signed bytes (-80..7F)"},
    {"unsigned",   "cs unsigned",            "Compare values as unsigned bytes (00..FF, the default)"},
    {"=",          "cs = value",             "Keep the bytes equal to value (hex)"},
    {"!=",         "cs != value",            "Keep the bytes not equal to value (hex)"},
    {"<",          "cs < value",             "Keep the bytes less than value (hex)"},
    {">",          "cs > value",             "Keep the bytes greater than value (hex)"},
    {"+",          "cs + n",                 "Keep the bytes that went up by exactly n (hex) while the game ran"},
    {"-",          "cs - n",                 "Keep the bytes that went down by exactly n (hex) while the game ran"},
    {"same",       "cs same",                "Keep the bytes that did not change while the game ran"},
    {"diff",       "cs diff",                "Keep the bytes that changed while the game ran (also: cs changed)"},
    {"up",         "cs up",                  "Keep the bytes that went up while the game ran"},
    {"down",       "cs down",                "Keep the bytes that went down while the game ran"},
    {"written",    "cs written",             "Keep the bytes the CPU wrote while the game ran (cpu only)"},
    {"notwritten", "cs notwritten",          "Keep the bytes the CPU did not write while the game ran (cpu only)"},
    {"read",       "cs read",                "Keep the bytes the CPU read while the game ran (cpu only)"},
    {"notread",    "cs notread",             "Keep the bytes the CPU did not read while the game ran (cpu only)"},
};
// clang-format on

std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// The help line while typing the arguments of a command with subcommands; empty if the command has none.
std::string subCommandHint(const std::string &command, std::string args)
{
    if (command != "cs") {
        return std::string();
    }
    args.erase(0, args.find_first_not_of(" \t"));
    std::string list; // "START, STOP, ..., = != < > + -, SAME, ..."
    for (const SubCommand &s : kSearchSubCommands) {
        const bool op = !std::isalpha(static_cast<unsigned char>(s.name[0]));
        const bool afterOp = !list.empty() && !std::isalpha(static_cast<unsigned char>(list.back()));
        list += (list.empty() ? "" : (op && afterOp) ? " " : ", ") + upper(s.name);
    }
    if (args.empty()) {
        return list;
    }
    // the subcommand word; an operator may be followed directly by its value ("cs <5")
    std::string word;
    bool complete = false; // something follows the word
    if (std::string("=!<>+-").find(args[0]) != std::string::npos) {
        for (const char *op : {"!=", "==", "=", "<", ">", "+", "-"}) {
            if (args.compare(0, std::strlen(op), op) == 0) {
                word = (std::string(op) == "==") ? "=" : op;
                complete = args.size() > std::strlen(op);
                break;
            }
        }
        if (word.empty()) {
            word = args.substr(0, 1); // a lone '!'
        }
    } else {
        size_t space = args.find_first_of(" \t");
        word = lower(args.substr(0, space));
        complete = (space != std::string::npos);
    }
    if (word == "changed") {
        word = "diff";
    }
    for (const SubCommand &s : kSearchSubCommands) {
        if (word == s.name) {
            return complete ? std::string("Syntax: ") + s.syntax : s.what;
        }
    }
    if (complete) {
        return "Unknown cs subcommand '" + word + "' (help cs)";
    }
    std::string matches;
    for (const SubCommand &s : kSearchSubCommands) {
        if (std::string(s.name).compare(0, word.size(), word) == 0) {
            matches += (matches.empty() ? "" : ", ") + upper(s.name);
        }
    }
    return matches.empty() ? "No cs subcommand starts with '" + word + "'" : matches;
}

std::optional<unsigned long> parseCount(const std::string &text)
{
    if (!text.empty() && text[0] == '$') {
        return parseHex(text);
    }
    char *end = nullptr;
    unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (text.empty() || *end != '\0') {
        return std::nullopt;
    }
    return value;
}

std::string currentDirectory()
{
    std::error_code ec;
    std::filesystem::path cwd = std::filesystem::current_path(ec);
    return ec ? std::string(".") : cwd.string();
}

// A file name typed after a command (lua, source): spaces kept, surrounding quotes taken off ("my file.lua" or
// my file.lua), ~ for the home directory. Relative names are from the current directory.
std::string fileArgument(const std::string &text)
{
    size_t first = text.find_first_not_of(" \t");
    size_t last = text.find_last_not_of(" \t");
    std::string file = (first == std::string::npos) ? std::string() : text.substr(first, last - first + 1);
    if (file.size() >= 2 && (file.front() == '"' || file.front() == '\'') && file.back() == file.front()) {
        file = file.substr(1, file.size() - 2);
    }
    if (!file.empty() && file[0] == '~' && (file.size() == 1 || file[1] == '/' || file[1] == '\\')) {
#ifdef _WIN32
        const char *home = std::getenv("USERPROFILE");
#else
        const char *home = std::getenv("HOME");
#endif
        if (home != nullptr) {
            file = home + file.substr(1);
        }
    }
    return file;
}

// The names in a directory that start with what was typed (TYPED: a path, maybe partial): directories with a '/'
// after them, files only with the extension EXT (if not empty). Sorted.
// EXT: one extension (".lua"), several separated by ';' (".nes;.zip"), or empty for every file
std::vector<std::string> fileMatches(const std::string &typed, const std::string &ext)
{
    auto hasExt = [&ext](const std::string &name) {
        if (ext.empty()) {
            return true;
        }
        std::istringstream list(ext);
        std::string one;
        while (std::getline(list, one, ';')) {
            if (name.size() > one.size() && lower(name.substr(name.size() - one.size())) == one) {
                return true;
            }
        }
        return false;
    };
    std::string path = fileArgument(typed);
    size_t slash = path.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? "." : (slash == 0 ? "/" : path.substr(0, slash));
    std::string start = (slash == std::string::npos) ? path : path.substr(slash + 1);
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.compare(0, start.size(), start) != 0 || (name[0] == '.' && (start.empty() || start[0] != '.'))) {
            continue;
        }
        std::error_code typeEc;
        if (it->is_directory(typeEc)) {
            names.push_back(name + "/");
        } else if (hasExt(name)) {
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Commands that work before a game is loaded (one-debugger started without a ROM)
bool needsRom(const std::string &name)
{
    static const char *const without[] = {"load", "source", "help", "h", "?", "q", "log", "fps", "lag", "version"};
    for (const char *n : without) {
        if (name == n) {
            return false;
        }
    }
    return true;
}

#if defined(__GNUC__)
#ifdef __MINGW32__
#define ONE_PRINTF_ARCHETYPE gnu_printf // (the C library is MinGW's own with __USE_MINGW_ANSI_STDIO: %zu, %llu)
#else
#define ONE_PRINTF_ARCHETYPE printf
#endif
std::string format(const char *fmt, ...) __attribute__((format(ONE_PRINTF_ARCHETYPE, 1, 2)));
#endif
std::string format(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

} // namespace

const std::vector<CommandProcessor::Command> &CommandProcessor::commands()
{
    // The "// @group" lines name the sections of the quick reference (docs/quickref/extract.sh reads this table).
    // (not reformatted: a table; docs/quickref/extract.sh reads it line by line)
    // clang-format off
    static const std::vector<Command> table = {
        // @group Flow Control
        {"t",     &CommandProcessor::cmdTrace,         {"Trace (step into) instructions.  Syntax: t [count]", "Func key: [F8]", nullptr}},
        {"p",     &CommandProcessor::cmdStep,          {"Program step: steps over subroutines.  Syntax: p [ret]", "Func key: [F10], [F12] for p ret", "p ret: go until the current subroutine returns (RTS or RTI) and stop in the caller", nullptr}},
        {"g",     &CommandProcessor::cmdGo,            {"Go.  Syntax: g [address]", "Runs until the address becomes the Program Counter, or a breakpoint is hit.", "Without an address: run until a breakpoint is hit.  [Ctrl+D] stops.", nullptr}},
        {"go",    &CommandProcessor::cmdGo,            {"Same as g", nullptr}},
        {"x",     &CommandProcessor::cmdExecute,       {"Execute: run until a breakpoint is hit.  Syntax: x", "Func key: [F5].  [Ctrl+D] stops.", nullptr}},
        {"here",  &CommandProcessor::cmdHere,          {"Go to the code window's cursor line.  Syntax: here", "Func key: [F7].  Runs until the instruction on the cursor line is reached (or a breakpoint is hit).", "The cursor must be in the code window: press F6 (ec) first and move it with the arrow keys.", nullptr}},
        {"gsl",   &CommandProcessor::cmdGoScanline,    {"Go to a scanline.  Syntax: gsl line", "Runs until the PPU reaches the scanline (decimal, 0-261).", nullptr}},
        {"fs",    &CommandProcessor::cmdFrameStep,     {"Frame step: run frames.  Syntax: fs [count]", "Runs COUNT frames (default 1, decimal) and stops at the start of vblank (scanline 241), where the next frame's NMI comes.", nullptr}},
        {"press", &CommandProcessor::cmdPress,         {"Press controller buttons.  Syntax: press [p2] buttons [frames]", "   Holds BUTTONS (a b select start up down left right, joined by +; - releases) on pad 1 (p2: pad 2) for FRAMES frames (default 2) from now: fs or x runs them.  Example: press a+right 30", nullptr}},
        {"zapper", &CommandProcessor::cmdZapper,       {"Aim the Zapper (light gun) on port 2.  Syntax: zapper [x y | off] [fire [frames]] | none", "   Plugs a Zapper into port 2 (instead of controller 2) and points it at the NES pixel X Y (0-255, 0-239), or away from the screen (off).  fire pulls the trigger for FRAMES frames (default 2) from now: fs or x runs them.  none unplugs it.  Without arguments: where it points.  The Zapper takes its aim once a frame, at the first read of $4017 after vblank.  Examples: zapper 136 104 fire 3   zapper off", nullptr}},
        {"savestate", &CommandProcessor::cmdSaveState, {"Save or load the game.  Syntax: savestate save|load [slot]", "   Slots 1-9, next to the ROM (game.ss1 ...), the same as F1/F2 in the game window; default: the current slot. save runs to the start of the next vblank (fs) and saves on the way, at the end of the picture.", nullptr}},
        // @group Breakpoints
        {"bpx",   &CommandProcessor::cmdBpx,           {"Breakpoint on execution.  Syntax: bpx address [if condition] [do \"commands\"]", "   Example: bpx $8432   or   bpx nmi_handler   or   bpx 8000 if a == 0 do \"d 300; g\"", "   if: Lua (a x y s p pc scanline cycle frame mem[addr] emu.*).  do: commands when it stops", "Func key: [F9] toggles a breakpoint on the code window's cursor line", nullptr}},
        {"bpm",   &CommandProcessor::cmdBpm,           {"Breakpoint on memory access.  Syntax: bpm address [r|w|rw|x] [if condition] [do \"commands\"]", "   Stops after the instruction that accessed the address.  Default: rw.  if: also address, value", nullptr}},
        {"bop",   &CommandProcessor::cmdBop,           {"Breakpoint on opcode execution.  Syntax: bop opcode [if condition] [do \"commands\"]", "   Example: bop 9E", nullptr}},
        {"bc",    &CommandProcessor::cmdBc,            {"Clear breakpoints.  Syntax: bc number... | *", "   Example: bc 3", nullptr}},
        {"bd",    &CommandProcessor::cmdBd,            {"Disable breakpoints.  Syntax: bd number... | *", nullptr}},
        {"be",    &CommandProcessor::cmdBe,            {"Enable breakpoints.  Syntax: be number... | *", nullptr}},
        {"bl",    &CommandProcessor::cmdBl,            {"List breakpoints.  Syntax: bl", nullptr}},
        // @group Display/Change Memory
        {"d",     &CommandProcessor::cmdDump,          {"Display CPU memory.  Syntax: d [address]", "   Without an address: the next block.  Addresses are hexadecimal.", nullptr}},
        {"dp",    &CommandProcessor::cmdDumpPpu,       {"Display PPU memory.  Syntax: dp [address]", "   Example: dp 3f00", nullptr}},
        {"u",     &CommandProcessor::cmdUnassemble,    {"Unassemble: show code at an address.  Syntax: u [address]", "   Without an address: back to the current instruction.", nullptr}},
        {"eb",    &CommandProcessor::cmdEditByte,      {"Edit bytes.  Syntax: eb address = value [value...]", "   Example: eb $10ff = a5 00 ff", nullptr}},
        {"eo",    &CommandProcessor::cmdEditOam,       {"Edit OAM (sprite memory).  Syntax: eo offset = value [value...]", "   Sprite n is at offset 4*n: Y, tile, attributes, X.  Example: eo 14 = 80 36 01 70", "   A game that copies its sprites with OAM DMA overwrites them next frame: edit the copy (eb).", nullptr}},
        {"r",     &CommandProcessor::cmdRegister,      {"Change a register.  Syntax: r register = value", "   Registers: pc a x y p sp.  Example: r a = $1f", nullptr}},
        {"n",     &CommandProcessor::cmdName,          {"Name (label) an address.  Syntax: n address [name]", "   Example: n $8000 rom_start.  Without a name: remove the label.  lsave saves the labels.", "   In switchable ROM, the label belongs to the bank mapped there now.", nullptr}},
        {"watch", &CommandProcessor::cmdWatch,         {"Watch memory in the watch window (shown at every stop).  Syntax: watch [-p] address [type] [name]", "   Types: b byte (default), w word, s signed byte, bin binary, l n: n bytes (1-16).  -p: PPU memory.", "   Example: watch 774 b health.  watch alone lists the watches.  Kept in the ROM's name with .watch.", nullptr}},
        {"wc",    &CommandProcessor::cmdWatchClear,    {"Clear watches.  Syntax: wc number... | *", "   The watch window closes when the last one goes.", nullptr}},
        {"comment", &CommandProcessor::cmdComment,     {"Comment an address (shown in the code window).  Syntax: comment address [text]", "   Without text: remove the comment.  \\n in the text starts a new line (shown as a space).", "   Key: [;] on the code window's cursor line.  Saved with the labels (lsave, .mlb).", nullptr}},
        {"lb",    &CommandProcessor::cmdLoadBinary,    {"Load a binary file into CPU memory.  Syntax: lb file address", "   Example: lb program.bin 0300", nullptr}},
        // @group Search and Labels
        {"s",     &CommandProcessor::cmdFind,          {"Search memory for data.  Syntax: s [-cp] [address L length data-list]", "   data-list: bytes and quoted strings, e.g. s c000 l 4000 a9 'HI',0   (length hex)", "   -c: strings ignore case.  -p: PPU memory.  s alone: continue after the last match.", "   A match is shown in the memory window.", nullptr}},
        {"cs",    &CommandProcessor::cmdSearch,        {"Memory search (cheat search): find a variable by how it changes.  Syntax: cs start [cpu|ppu|oam]", "Then, after letting the game run: cs = v, cs != v, cs < v, cs > v, cs same, cs diff, cs up, cs down,", "cs +n, cs -n (values hex), cs written, cs notwritten, cs read, cs notread (cpu only).", "cs list [n] shows the candidates, cs undo takes back a filter, cs signed|unsigned,", "cs reset starts over on the same memory, cs stop ends the search.", "Filters typed one after another judge the same stretch of play (e.g. cs same, then cs notwritten).", nullptr}},
        {"freeze", &CommandProcessor::cmdFreeze,       {"Freeze a RAM byte: written back every frame.  Syntax: freeze [address [value]]", "Without a value: freeze at the current value.  Without an address: list the frozen bytes.", nullptr}},
        {"unfreeze", &CommandProcessor::cmdUnfreeze,   {"Stop freezing.  Syntax: unfreeze address | *", nullptr}},
        {"prof",  &CommandProcessor::cmdProfile,       {"Profiler: calls and cycles per routine.  Syntax: prof [on|off|reset|list [sort COLUMN] [asc] [N]]", "   on starts counting (call tracking runs while it does), off stops, reset forgets the counts.  list (the", "   default) prints the top N (20) routines, sorted by excl (cycles of the routine itself) unless sort says", "   name|address|calls|incl|incl%|excl|excl%|avg|min|max.  Incl includes the routines called, not interrupts.", "   [root] is the time outside every JSR.  wprof opens the same table in a window."}},
        {"bt",    &CommandProcessor::cmdBacktrace,     {"Call stack: how the CPU got here.  Syntax: bt [jumps]", "   The routines that have been called and not returned (JSR, interrupts), innermost first, and how the", "   current instruction was reached (a branch or JMP).  bt jumps: the last jumps and taken branches.", "   Call tracking starts with the first bt (or cdl on); calls made before, or before a state load, are guessed from the", "   stack contents (marked 'guessed')."}},
        {"cdl",   &CommandProcessor::cmdCdl,           {"Code/data log (Mesen CDL format).  Syntax: cdl [on|off|clear|stats|unseen [bank [bytes]]|mark|unmark address [length]|save [file]|load [file] [force]]", "   Records which PRG-ROM bytes ran as code, were read as data, or were jumped to (kept per bank).", "   stats: how much is known, per bank.  unseen: the ranges never seen.  save / load: game.cdl by default.", "   mark / unmark: tell the log by hand that bytes are data (length in hex): the code window shows data as .db rows.", "   game.cdl is loaded with the game, saved by cdl off and when quitting with the log on (or by cdl save).", "   Mesen can load the file, and the other way round."}},
        {"lsave", &CommandProcessor::cmdLabelSave,     {"Save the labels.  Syntax: lsave [file]", "   Mesen .mlb format.  Without a file: the ROM's name with .mlb (game.nes -> game.mlb),", "   which is loaded automatically next time.", nullptr}},
        {"lload", &CommandProcessor::cmdLabelLoad,     {"Load labels (added to the current ones).  Syntax: lload file", "   Mesen .mlb, ld65 debug info (ld65 --dbgfile) or ld65 -Ln / VICE labels.", nullptr}},
        {"gg",    &CommandProcessor::cmdGameGenie,     {"Game Genie codes: decode, encode, apply.  Syntax: gg code | gg address value [compare] | gg on code [description] | gg off code|* | gg delete code|* | gg list | gg db [word|N] | gg enable | gg disable", "   gg SXIOPO (or gg decode SXIOPO) decodes, gg 91d9 ad encodes. gg on SXIOPO (or gg apply SXIOPO) puts a code in the game's list and ticks it: the game reads its value at its address (an 8-letter code only where the ROM has its compare value). gg off unticks it (* all), gg delete takes it out of the list, gg list shows the list, gg disable / enable switches Game Genie off and on for the game. gg db lists the known codes for the game (gg db lives: those about lives), gg db N ticks one. The list is remembered per game.", nullptr}},
        // @group Windows
        {"ec",    &CommandProcessor::cmdEnterCode,     {"Enter/exit the code window.  Syntax: ec", "Func key: [F6].  The arrow keys and PgUp/PgDn then move the code cursor, F9 toggles", "a breakpoint on the cursor line, and F6 again returns to the command line.", nullptr}},
        {"ww",    &CommandProcessor::cmdWatchWindow,   {"Open or close the watch window.  Syntax: ww", "   In the window: Del removes a watch, Enter edits its value (eb), Ins adds one,", "   Ctrl+F freezes or unfreezes it.", nullptr}},
        {"woam",  &CommandProcessor::cmdOam,           {"Open or close the OAM (sprites) window.  Syntax: woam", "   The 64 sprites decoded, with warnings: lines with more than 8 sprites, sprite 0's hit, masked", "   sprites.  Keys: A all 64 / visible ones, H bytes / decoded, Enter edits (eo).  oam prints it.", nullptr}},
        {"oam",   &CommandProcessor::cmdOamList,       {"Show the sprites (OAM) decoded.  Syntax: oam [all]", "   Without all: the visible ones.  As the OAM window (woam).", nullptr}},
        {"rs",    &CommandProcessor::cmdRestoreScreen, {"Frame in progress in the game window, at every stop.  Syntax: rs [on|off]", "   Shows what the PPU has drawn so far; the rest, still the previous frame, is dimmed, and the", "   beam is marked.  rs alone switches it.", "Func key: [F4]", nullptr}},
        {"wbg",   &CommandProcessor::cmdBackground,    {"Open or close the background window.  Syntax: wbg [save file.png]", "   The four nametables; the part the game window shows in a white outline (each line's own scroll),", "   the rest dimmed.  O in it: one rectangle / each line.  wbg save FILE writes a 512x480 PNG.", nullptr}},
        {"wprof", &CommandProcessor::cmdProfilerWindow,{"Open or close the profiler window.  Syntax: wprof [save file.png]", "   Routines with their calls and cycles (see prof); opening it starts the profiler, closing it stops it.", "   Sort with the keys 1-9, 0 (name, address, calls, incl, incl%, excl, excl%, avg, min, max; again reverses),", "   Left/Right; Up/Down/PgUp/PgDn scroll; F per frame; R reset.  wprof save FILE writes a PNG.", nullptr}},
        {"wpal",  &CommandProcessor::cmdPalette,       {"Open or close the palette window.  Syntax: wpal [save file.png]", "   Palette RAM (BG0-3, SP0-3, with values) and the 64 NES colors, the ones in use outlined; the mouse", "   shows details.  Live while the game runs.  wpal save FILE writes it as a PNG.", nullptr}},
        {"wpat",  &CommandProcessor::cmdPatterns,      {"Open or close the pattern table window.  Syntax: wpat [save file.png]", "   Both pattern tables ($0000, $1000) through the CHR banks of now.  Keys in it: P palette (BG0-3,", "   SP0-3, gray), G grid; the mouse shows the tile.  wpat save FILE writes it as a PNG.", nullptr}},
        {"screenshot", &CommandProcessor::cmdScreenshot, {"Save the game picture as a PNG.  Syntax: screenshot [file]", "Func key: [Shift+F12], also in the game window.  Without a file: next to the ROM,", "   numbered (game-001.png, game-002.png, ...).  The last complete frame, 256x240.", nullptr}},
        {"lag",   &CommandProcessor::cmdLag,           {"Lag frame counter (FCEUX's).  Syntax: lag [on|off|reset]", "   A lag frame is a frame in which the game did not read the controllers.  Without an argument: the count.", "   on / off: show it on the game picture (\"Lag 123\", red on a frame that lagged), as the in-game menu does.", "   reset: zero the count (power-on does too; a soft reset does not).  It is part of a save state.", nullptr}},
        {"version", &CommandProcessor::cmdVersion,     {"Show ONE's version.  Syntax: version", "   The number, and for a build that is not a tagged release the commit it was built from (\"+g1a2b3c4\"; \"-dirty\" if the tree had changes).", nullptr}},
        {"fps",   &CommandProcessor::cmdFps,           {"Frames per second in the game window's title.  Syntax: fps [on|off]", "Func key: [Shift+F10], also in the game window.  Without on/off: switch it.", nullptr}},
        // @group Mode Control
        {"reset", &CommandProcessor::cmdReset,         {"Reset the NES.  Syntax: reset [hard]", "   reset: the RESET button.  reset hard: power cycle", nullptr}},
        {"q",     &CommandProcessor::cmdQuit,          {"Quit the debugger.  Syntax: q", nullptr}},
        // @group Miscellaneous
        {"log",   &CommandProcessor::cmdLog,           {"Emulator messages: how much each part tells.  Syntax: log [category] [level]", "   Levels: off error warn info debug trace.  Categories: emu cpu ppu apu dma mapper input", "   video audio savestate cfg script.  log alone lists them.  Example: log ppu debug.  Debug and trace", "   messages go to the log file only.", nullptr}},
        {"load",  &CommandProcessor::cmdLoad,          {"Load a game (power it on).  Syntax: load file.nes  (or file.zip: its first .nes)", "   Its labels (.mlb), watches (.watch) and init file (.init) come with it.  Breakpoints are", "   cleared.  Tab completes the name.  Menu: File > Open game (F3), and the recent games.", nullptr}},
        {"source", &CommandProcessor::cmdSource,      {"Run debugger commands from a file.  Syntax: source file", "   One command a line, '#' starts a comment.  A command that runs the game (g, x, t, p...)", "   finishes before the next line.  game.init next to the ROM runs at start-up (not with --batch).", nullptr}},
        {"lua",   &CommandProcessor::cmdLua,           {"Lua scripts (Mesen 2 and FCEUX APIs).  Syntax: lua [file | stop [n]]", "   lua file.lua: run a script (from the current directory, or a full path; spaces are fine, Tab", "   completes).  lua alone: the running scripts.  lua stop: end them all, lua stop 1: one.  Menu: Lua.", "   print() and errors appear here.  emu.stop() quits the debugger, emu.breakExecution() stops.", "   debugger.command(\"bl\") runs a command and returns its output.", nullptr}},
        {"h",     &CommandProcessor::cmdHelp,          {"Help.  Syntax: help [command]", "   The following are equivalent: help h ? [F1]", nullptr}},
        {"help",  &CommandProcessor::cmdHelp,          {"Help.  Syntax: help [command]", nullptr}},
        {"?",     &CommandProcessor::cmdHelp,          {"Help.  Syntax: help [command]", nullptr}},
    };
    // clang-format on
    return table;
}

namespace {
CommandProcessor *activeProcessor = nullptr; // Lua's debugger.command() runs its commands here
}

CommandProcessor::CommandProcessor(CommandHost &host, Symbols &symbols) : host(host), symbols(symbols)
{
    activeProcessor = this;
    script_set_debugger_hooks(
        [](const char *line) -> char * {
            std::string output = activeProcessor ? activeProcessor->executeCaptured(line) : std::string();
            char *copy = static_cast<char *>(std::malloc(output.size() + 1)); // (freed by the script code with free())
            if (copy != nullptr) {
                std::memcpy(copy, output.c_str(), output.size() + 1);
            }
            return copy;
        },
        [] { Emulator::instance().stop(); },
        [](const char *label) -> int {
            if (activeProcessor == nullptr) {
                return -1;
            }
            std::optional<uint16_t> address = activeProcessor->symbols.find(label);
            return address ? *address : -1;
        });
}

CommandProcessor::~CommandProcessor()
{
    if (activeProcessor == this) {
        activeProcessor = nullptr;
    }
}

void CommandProcessor::print(const std::string &line)
{
    if (capture != nullptr) {
        if (!capture->empty()) {
            *capture += '\n';
        }
        *capture += line;
    } else {
        host.print(line);
        if (tee != nullptr) {
            *tee += line + "\n";
        }
    }
}

std::string CommandProcessor::executeCaptured(const std::string &line)
{
    std::string output;
    std::string *outer = capture;
    std::string saved = rawArguments;
    capture = &output;
    execute(line);
    capture = outer;
    rawArguments = saved;
    return output;
}

void CommandProcessor::enqueue(const std::vector<std::string> &lines, bool atFront)
{
    if (atFront) {
        pending.insert(pending.begin(), lines.begin(), lines.end());
    } else {
        pending.insert(pending.end(), lines.begin(), lines.end());
    }
}

std::string CommandProcessor::takePending()
{
    std::string line = pending.front();
    pending.pop_front();
    return line;
}

bool CommandProcessor::initFiles = true;

bool CommandProcessor::nextNeedsRom() const
{
    if (pending.empty() || Emulator::instance().hasRom()) {
        return false;
    }
    std::vector<std::string> words = split(pending.front());
    return !words.empty() && words[0][0] != '#' && words[0][0] != '!' && needsRom(lower(words[0]));
}

std::string CommandProcessor::romSibling(const std::string &rom, const char *extension)
{
    std::string base = rom;
    size_t dot = base.find_last_of('.');
    size_t slash = base.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        base.erase(dot);
    }
    return base + extension;
}

void CommandProcessor::cmdLoad(const Args &args)
{
    std::string file = fileArgument(rawArguments);
    if (args.size() < 2 || file.empty()) {
        print("Syntax: load file.nes");
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        print("No file '" + file + "' (relative names are from " + currentDirectory() + "; Tab completes file names)");
        return;
    }
    if (remote != nullptr) {
        if (std::string why = remote->romRefusal(file); !why.empty()) {
            print(why);
            return;
        }
    }
    loadRom(file);
}

std::string CommandProcessor::loadSavedCodeLog()
{
    Emulator &emu = Emulator::instance();
    const std::string file = romSibling(emu.romPath(), ".cdl");
    if (!emu.hasRom() || !std::ifstream(file)) {
        return std::string();
    }
    const std::string error = emu.codeLog().load(file, false);
    return error.empty() ? "Code/data log from " + file : "Cannot load " + file + ": " + error;
}

void CommandProcessor::saveCodeLogIfOn()
{
    Emulator &emu = Emulator::instance();
    if (emu.hasRom() && emu.codeLog().enabled()) {
        emu.codeLog().save(romSibling(emu.romPath(), ".cdl")); // (silent: nobody is left to read an error)
    }
}

bool CommandProcessor::loadRom(const std::string &file)
{
    Emulator &emu = Emulator::instance();
    if (emu.insideCpu() || script_in_callback()) {
        print("Cannot load a game from inside a Lua callback");
        return false;
    }
    // (before the current game is switched off)
    if (zip_is_zip_name(file.c_str())) {
        uint8_t *rom = nullptr;
        size_t romSize = 0;
        char entry[512], error[128];
        if (!zip_extract_nes(file.c_str(), &rom, &romSize, entry, sizeof(entry), error, sizeof(error))) {
            print("'" + file + "': " + error);
            return false;
        }
        const bool ines = romSize >= 4 && std::memcmp(rom, "NES\x1A", 4) == 0;
        std::free(rom);
        if (!ines) {
            print("'" + file + "': " + entry + " is not an iNES file");
            return false;
        }
    } else {
        char magic[4] = {};
        std::ifstream(file, std::ios::binary).read(magic, sizeof(magic));
        if (std::string(magic, sizeof(magic)) != std::string("NES\x1A", 4)) {
            print("'" + file + "' is not an iNES file (.nes)");
            return false;
        }
    }
    if (!host.confirmDiscardLabels()) {
        return false;
    }
    if (host.interactive()) {
        saveCodeLogIfOn(); // (the log of the game that is leaving)
    }
    emu.removeAllBreakpoints(); // (addresses of the other game)
    emu.unfreezeAll();
    if (!emu.powerOn(file)) {
        print("Cannot load '" + file + "' (see the log file).  No game loaded.");
        host.romLoaded();
        return false;
    }
    // its labels and comments (game.mlb, where lsave writes)
    symbols.clear();
    std::string labels = romSibling(file, ".mlb");
    symbols.setFileName(labels);
    if (std::ifstream(labels)) {
        int count = symbols.loadFile(labels);
        if (count >= 0) {
            print(std::to_string(count) + (count == 1 ? " label" : " labels") + " from " + labels);
        }
    }
    // the assembler's debug info (game.dbg from ld65 --dbgfile), rebuilt with the ROM: read-only, never saved
    const std::string dbg = romSibling(file, ".dbg");
    if (std::ifstream(dbg)) {
        int count = symbols.loadFile(dbg);
        if (count >= 0) {
            print(std::to_string(count) + (count == 1 ? " label" : " labels") + " from " + dbg);
        }
    }
    symbols.setModified(false);
    // its watches (game.watch; not with --batch)
    Watches::instance().clear();
    if (host.interactive()) {
        Watches::instance().setFileName(romSibling(file, ".watch"));
        loadWatches();
        fe_recent_add(FE_RECENT_GAMES, file.c_str());
    }
    host.watchesChanged(false);
    if (host.interactive()) {
        const std::string logMessage = loadSavedCodeLog();
        if (!logMessage.empty()) {
            print(logMessage);
        }
    }
    print("Loaded " + file);
    host.romLoaded();
    host.stopped();
    // its init file (game.init) runs next
    std::string init = romSibling(file, ".init");
    if (initFiles && std::ifstream(init)) {
        print("Commands from " + init);
        enqueue({"source " + init}, true);
    }
    return true;
}

bool CommandProcessor::canRun()
{
    if (!Emulator::instance().hasRom()) {
        print("No game loaded: load file.nes (or File > Open game, F3)");
        return false;
    }
    if (Emulator::instance().insideCpu()) {
        print("Cannot run the game from inside a Lua callback (the game is running already)");
        return false;
    }
    return true;
}

void CommandProcessor::cmdSource(const Args &args)
{
    std::string file = fileArgument(rawArguments); // (a file name may contain spaces)
    if (args.size() < 2 || file.empty()) {
        print("Syntax: source file");
        return;
    }
    std::ifstream in(file);
    if (!in) {
        print("Cannot open '" + file + "' (relative names are from " + currentDirectory() + ")");
        return;
    }
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    if (pending.size() + lines.size() > 100000) {
        print("source: too many commands waiting (does '" + file + "' source itself?)");
        return;
    }
    enqueue(lines, true); // (before what was waiting: a sourced file runs where it was sourced)
}

void CommandProcessor::execute(const std::string &rawLine)
{
    std::string line = rawLine;
    size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#') {
        return;
    }
    line = line.substr(first);
    if (capture == nullptr) {
        print(":" + line);
    }
    if (line.compare(0, 10, "!complete ") == 0) {
        // (tests) !complete TEXT: print what Tab makes of TEXT; !complete "TEXT" "EXPECTED": assert it
        std::string rest = rawLine.substr(rawLine.find("!complete ") + 10);
        std::vector<std::string> quoted;
        for (size_t open = rest.find('"'); open != std::string::npos; open = rest.find('"', open)) {
            size_t close = rest.find('"', open + 1);
            if (close == std::string::npos) {
                break;
            }
            quoted.push_back(rest.substr(open + 1, close - open - 1));
            open = close + 1;
        }
        if (quoted.size() == 2) {
            std::string got = complete(quoted[0]);
            if (got != quoted[1]) {
                failedAssertions++;
                print("ASSERTION FAILED: Tab on '" + quoted[0] + "' gave '" + got + "', expected '" + quoted[1] + "'");
            }
        } else {
            print("completed: '" + complete(rest) + "'");
        }
        return;
    }
    if (remote != nullptr) {
        if (std::string why = RemoteServer::refusal(line); !why.empty()) {
            print(why);
            return;
        }
    }
    if (line[0] == '!') {
        scriptCommand(split(line.substr(1)));
        return;
    }
    Args args = split(line);
    std::string name = lower(args[0]);
    if (!Emulator::instance().hasRom() && needsRom(name)) {
        print("No game loaded: load file.nes (or File > Open game, F3)");
        return;
    }
    size_t afterName = line.find_first_of(" \t");
    rawArguments = (afterName == std::string::npos) ? std::string() : line.substr(afterName);
    for (const Command &command : commands()) {
        if (name == command.name) {
            (this->*command.run)(args);
            return;
        }
    }
    print("Unknown command.  'help' lists the commands.");
}

std::string CommandProcessor::complete(const std::string &partial) const
{
    // the word being typed: after the last space (empty if the line ends with a space)
    size_t wordStart = partial.find_last_of(" \t");
    wordStart = (wordStart == std::string::npos) ? 0 : wordStart + 1;
    const std::string word = lower(partial.substr(wordStart));
    std::vector<std::string> before = split(partial.substr(0, wordStart)); // the words already typed
    for (std::string &w : before) {
        w = lower(w);
    }

    // lua / source: a file name (spaces and all: everything after the command)
    size_t first = partial.find_first_not_of(" \t");
    size_t afterName = partial.find_first_of(" \t", first == std::string::npos ? 0 : first);
    if (!before.empty() && (before[0] == "lua" || before[0] == "source" || before[0] == "load") &&
        afterName != std::string::npos) {
        size_t nameStart = partial.find_first_not_of(" \t", afterName); // (none yet: "lua " -- every file)
        std::string typed = (nameStart == std::string::npos) ? std::string() : partial.substr(nameStart);
        std::vector<std::string> names =
            fileMatches(typed, before[0] == "lua" ? ".lua" : (before[0] == "load" ? ".nes;.zip" : ""));
        if (names.empty()) {
            return partial;
        }
        std::string common = names[0];
        for (const std::string &n : names) {
            size_t k = 0;
            while (k < common.size() && k < n.size() && common[k] == n[k]) {
                k++;
            }
            common.resize(k);
        }
        size_t slash = typed.find_last_of('/');
        std::string completed = (slash == std::string::npos ? std::string() : typed.substr(0, slash + 1)) + common;
        return partial.substr(0, partial.size() - typed.size()) + completed;
    }

    std::vector<std::string> choices;
    if (before.empty() || (before.size() == 1 && before[0] == "help")) {
        for (const Command &c : commands()) {
            choices.emplace_back(c.name);
        }
    } else if (before.size() == 1 && before[0] == "cs") {
        for (const SubCommand &s : kSearchSubCommands) {
            if (std::isalpha(static_cast<unsigned char>(s.name[0]))) { // (not the operators: nothing to complete)
                choices.emplace_back(s.name);
            }
        }
    } else if (before.size() == 2 && before[0] == "cs" && before[1] == "start") {
        choices = {"cpu", "ppu", "oam"};
    }
    std::vector<std::string> matches;
    for (const std::string &c : choices) {
        if (c.compare(0, word.size(), word) == 0) {
            matches.push_back(c);
        }
    }
    if (matches.empty() || partial.empty() || (word.empty() && matches.size() > 1)) {
        return partial;
    }
    if (matches.size() == 1) {
        return partial.substr(0, wordStart) + matches[0] + " ";
    }
    std::string common = matches[0]; // several: as far as they agree
    for (const std::string &m : matches) {
        size_t n = 0;
        while (n < common.size() && n < m.size() && common[n] == m[n]) {
            n++;
        }
        common.resize(n);
    }
    return partial.substr(0, wordStart) + common;
}

std::string CommandProcessor::hint(const std::string &partial) const
{
    size_t start = partial.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return std::string();
    }
    std::string text = partial.substr(start);
    if (text[0] == '!') {
        return "Script commands: !assert reg r value, !assert mem address value, !assert previous address expected, !assert backtrack address lines expected, !assert watch n value, !assert comment address text, !assert oam n text, !assert frame n, !assert scanline n, !assert pad 1|2 buttons, !assert cdl code|data|jump|sub|unseen offset, !assert cdlpcm min n, !assert bt depth|site|target ..., !assert prof calls|incl|excl|min|max|routines|balanced ..., !assert lag [min] n | lag last 0|1, !set reg r value";
    }
    if (text[0] == '#') {
        return "Comment";
    }
    size_t space = text.find_first_of(" \t");
    const std::string word = lower(text.substr(0, space));
    const bool typingArguments = (space != std::string::npos);

    // the command, following "Same as g" aliases
    auto find = [](const std::string &name) -> const Command * {
        for (const Command &c : commands()) {
            if (name == c.name) {
                return &c;
            }
        }
        return nullptr;
    };
    const Command *exact = find(word);
    if (exact && std::strncmp(exact->help[0], "Same as ", 8) == 0) {
        exact = find(exact->help[0] + 8);
    }
    if (exact) {
        std::string first = exact->help[0];
        size_t syntax = first.find("  Syntax: ");
        std::string what = (syntax == std::string::npos) ? first : first.substr(0, syntax);
        if (!what.empty() && what.back() == '.') {
            what.pop_back();
        }
        if (typingArguments) {
            std::string sub = subCommandHint(exact->name, text.substr(space));
            if (!sub.empty()) {
                return sub;
            }
        }
        if (typingArguments &&
            (word == "lua" || word == "source" || word == "load")) { // the files that fit what is typed
            size_t from = text.find_first_not_of(" \t", space);
            std::string typed = (from == std::string::npos) ? std::string() : text.substr(from);
            if (!typed.empty() && lower(typed).compare(0, 4, "stop") != 0) {
                std::vector<std::string> names =
                    fileMatches(typed, word == "lua" ? ".lua" : (word == "load" ? ".nes;.zip" : ""));
                if (names.empty()) {
                    return "No such file here (relative names are from " + currentDirectory() + ")";
                }
                std::string list = "Tab completes:";
                for (const std::string &n : names) {
                    if (list.size() + n.size() > 200) {
                        list += "  ...";
                        break;
                    }
                    list += "  " + n;
                }
                return list;
            }
        }
        if (typingArguments && syntax != std::string::npos) {
            return first.substr(syntax + 2); // "Syntax: bpm address [r|w|rw|x]"
        }
        return what;
    }
    if (typingArguments) {
        return "Unknown command '" + word + "'";
    }
    std::string matches;
    for (const Command &c : commands()) {
        if (std::string(c.name).compare(0, word.size(), word) == 0) {
            matches += (matches.empty() ? "" : ", ") + upper(c.name);
        }
    }
    return matches.empty() ? "No command starts with '" + word + "'" : matches;
}

std::string CommandProcessor::unknownAddress(const std::string &text) const
{
    if (const Symbols::Location *location = symbols.locationOf(text)) {
        return "'" + text + "' is at " + Symbols::describe(*location) + ", which is not mapped in right now";
    }
    return "Unknown symbol or address '" + text + "'";
}

std::optional<uint16_t> CommandProcessor::parseAddress(const std::string &text) const
{
    if (auto value = parseHex(text)) {
        if (*value <= 0xFFFF) {
            return static_cast<uint16_t>(*value);
        }
        return std::nullopt;
    }
    return symbols.find(text);
}

void CommandProcessor::reportStop(StopReason reason)
{
    Emulator &emu = Emulator::instance();
    switch (reason) {
    case StopReason::Breakpoint: {
        int slot = emu.hitBreakpoint();
        if (slot >= 0 && static_cast<unsigned>(slot) < emu.breakpoints().size()) {
            const Breakpoint &bp = emu.breakpoints()[slot];
            if (bp.type == Breakpoint::Type::Execute) {
                print(format("Break due to BPX #%d", slot) + stopLocation());
            } else if (bp.type == Breakpoint::Type::Opcode) {
                print(format("Break due to BOP #%d (opcode $%02X)", slot, bp.address) + stopLocation());
            } else {
                print(format("Break due to BPM #%d ($%04X %s)", slot, emu.hitAccessAddress(),
                             emu.hitAccessWasWrite() ? "written" : "read") +
                      stopLocation());
            }
        } else {
            print("Break due to a breakpoint");
        }
        break;
    }
    case StopReason::Scanline: print(format("Scanline %u reached", ppu_debug_get_scanline())); break;
    case StopReason::Frames:
        if (pendingSaveSlot > 0) { // savestate save: the save was taken on the way (end of the picture)
            print(savestate_save_requested() ? format("Slot %d: not saved", pendingSaveSlot)
                                             : format("Saved to slot %d", pendingSaveSlot));
            pendingSaveSlot = 0;
        }
        print(format("Frame %u: the start of vblank", ppu_debug_get_frame_num()));
        break;
    case StopReason::Return: print("Return reached"); break;
    case StopReason::UserBreak: print("Stopped"); break;
    case StopReason::Jammed:
        print(format("CPU jammed: opcode $%02X at $%04X is a JAM (or not implemented).  Change PC or reset.",
                     emu.jamOpcode(), cpu_debug_get_pc()));
        break;
    case StopReason::Ended:
        print("The emulation has ended (game window closed?).  'reset hard' powers on again.");
        break;
    case StopReason::Step:
    case StopReason::None: break;
    }
    host.stopped();
    script_event(SCRIPT_EVENT_CODE_BREAK); // scripts: the debugger stopped
    // bpx ... do "COMMANDS": they run next (before anything else waiting), one at a time like a source file
    int slot = emu.hitBreakpoint();
    if (reason == StopReason::Breakpoint && slot >= 0 && static_cast<unsigned>(slot) < emu.breakpoints().size()) {
        const std::string &action = emu.breakpoints()[slot].action;
        std::vector<std::string> lines;
        size_t start = 0;
        while (start <= action.size()) {
            size_t end = action.find(';', start);
            if (end == std::string::npos) {
                end = action.size();
            }
            std::string command = action.substr(start, end - start);
            if (command.find_first_not_of(" \t") != std::string::npos) {
                lines.push_back(command);
            }
            start = end + 1;
        }
        enqueue(lines, true);
    }
}

// --- Lua scripts -------------------------------------------------------------------------------------------------

namespace {
CommandHost *scriptStopHost = nullptr; // emu.stop() quits the debugger
}

void CommandProcessor::cmdLua(const Args &args)
{
    if (args.size() == 1) {
        if (script_count() == 0) {
            print("No Lua scripts running.  Syntax: lua file.lua | lua stop [n]");
        }
        for (int i = 0; i < script_count(); i++) {
            print(format("  %d %s  (%s)", i, script_name(i), script_path(i)));
        }
        print("Relative file names are from " + currentDirectory());
        return;
    }
    if (Emulator::instance().insideCpu() || script_in_callback()) {
        print("Cannot start or stop Lua scripts from inside a Lua callback");
        return;
    }
    if (args.size() == 2 && lower(args[1]) == "stop") {
        script_stop_all();
        return;
    }
    if (args.size() == 3 && lower(args[1]) == "stop") {
        auto n = parseCount(args[2]);
        if (!n || !script_stop(static_cast<int>(*n))) {
            print("No Lua script #" + args[2] + " ('lua' lists them)");
        }
        return;
    }
    std::string file = fileArgument(rawArguments); // (a file name may contain spaces)
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        print("No file '" + file + "' (relative names are from " + currentDirectory() + "; Tab completes file names)");
        return;
    }
    scriptStopHost = &host;
    script_set_stop_hook([](int) {
        Emulator::instance().stop();
        if (scriptStopHost != nullptr) {
            scriptStopHost->quit();
        }
    });
    script_load(file.c_str()); // (messages and errors arrive through the log)
    host.stateChanged();       // (the script may have written memory)
}

// --- execution ---------------------------------------------------------------------------------------------------

void CommandProcessor::stepInto(unsigned count)
{
    if (!canRun()) {
        return;
    }
    reportStop(Emulator::instance().stepInto(count));
}

void CommandProcessor::stepOver()
{
    if (!canRun()) {
        return;
    }
    reportStop(Emulator::instance().stepOver());
}

void CommandProcessor::stepToReturn()
{
    if (!canRun()) {
        return;
    }
    reportStop(Emulator::instance().stepToReturn());
}

void CommandProcessor::go()
{
    if (!canRun()) {
        return;
    }
    if (Emulator::instance().ended()) {
        reportStop(StopReason::Ended);
        return;
    }
    Emulator::instance().run();
    host.runStarted();
}

void CommandProcessor::cmdTrace(const Args &args)
{
    unsigned count = 1;
    if (args.size() > 1) {
        auto value = parseCount(args[1]);
        if (!value || *value == 0) {
            print("Invalid count");
            return;
        }
        count = static_cast<unsigned>(*value);
    }
    stepInto(count);
}

void CommandProcessor::cmdStep(const Args &args)
{
    if (args.size() > 1) {
        if (lower(args[1]) == "ret") {
            stepToReturn();
        } else {
            print("Command 'p': unrecognized parameter '" + args[1] + "'");
        }
        return;
    }
    stepOver();
}

void CommandProcessor::cmdGo(const Args &args)
{
    if (args.size() > 2) {
        print("Command 'g': unrecognized parameter '" + args[2] + "'");
        return;
    }
    if (!canRun()) {
        return;
    }
    if (Emulator::instance().ended()) {
        reportStop(StopReason::Ended);
        return;
    }
    if (args.size() == 2) {
        auto address = parseAddress(args[1]);
        if (!address) {
            print(unknownAddress(args[1]));
            return;
        }
        Emulator::instance().goTo(*address);
        host.runStarted();
        return;
    }
    go();
}

void CommandProcessor::cmdExecute(const Args &args)
{
    if (args.size() > 1) {
        print("Command 'x': unrecognized parameter '" + args[1] + "'");
        return;
    }
    go();
}

// SoftICE HERE (F7): run to the code window's cursor line
void CommandProcessor::cmdHere(const Args &args)
{
    if (args.size() > 1) {
        print("Command 'here': unrecognized parameter '" + args[1] + "'");
        return;
    }
    if (!canRun()) {
        return;
    }
    std::optional<uint16_t> cursor = host.codeCursor();
    if (!cursor) {
        print("The cursor is not in the code window: press F6 (ec) first");
        return;
    }
    if (Emulator::instance().ended()) {
        reportStop(StopReason::Ended);
        return;
    }
    Emulator::instance().goTo(*cursor);
    host.runStarted();
}

void CommandProcessor::cmdFrameStep(const Args &args)
{
    auto count = (args.size() == 2) ? parseCount(args[1]) : std::optional<unsigned long>(1);
    if (args.size() > 2 || !count || *count < 1 || *count > 1000000) {
        print("Syntax: fs [count]   (frames, decimal; default 1)");
        return;
    }
    if (!canRun()) {
        return;
    }
    if (Emulator::instance().ended()) {
        reportStop(StopReason::Ended);
        return;
    }
    Emulator::instance().runFrames(static_cast<unsigned>(*count));
    host.runStarted();
}

void CommandProcessor::cmdPress(const Args &args)
{
    size_t i = 1;
    int port = 0;
    if (i < args.size() && lower(args[i]) == "p2") {
        port = 1;
        i++;
    }
    uint8_t buttons = 0;
    if (i >= args.size() || args.size() - i > 2 || !input_parse_buttons(lower(args[i]).c_str(), &buttons)) {
        print("Syntax: press [p2] buttons [frames]   (buttons: a b select start up down left right, joined by +)");
        return;
    }
    auto frames = (i + 1 < args.size()) ? parseCount(args[i + 1]) : std::optional<unsigned long>(2);
    if (!frames) {
        print("Frames: a decimal number");
        return;
    }
    constexpr uint64_t kFrameCycles = 29781; // CPU cycles per frame (29780.67)
    input_hold(port, buttons, cpu_debug_get_cycle() + *frames * kFrameCycles);
    std::string names = args[i];
    for (char &c : names) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (buttons == 0) {
        print(format("Pad %d: buttons released", port + 1));
    } else {
        print(format("Pad %d: %s held for %lu frame%s (fs or x runs them)", port + 1, names.c_str(), *frames,
                     *frames == 1 ? "" : "s"));
    }
}

void CommandProcessor::cmdZapper(const Args &args)
{
    constexpr uint64_t kFrameCycles = 29781; // CPU cycles per frame (29780.67)
    const char *syntax = "Syntax: zapper [x y | off] [fire [frames]] | none   (x 0-255, y 0-239)";
    if (args.size() == 1) {
        input_zapper_state now;
        if (!input_zapper_present() || !input_zapper_aim(&now)) {
            print("No Zapper (zapper x y plugs one into port 2)");
        } else if (now.on_screen) {
            print(format("Zapper: pointing at %d,%d%s", now.x, now.y, now.trigger ? ", trigger pulled" : ""));
        } else {
            print(format("Zapper: pointing away from the screen%s", now.trigger ? ", trigger pulled" : ""));
        }
        return;
    }
    std::string first = lower(args[1]);
    if (first == "none") {
        input_set_zapper(nullptr);
        input_zapper_pull_trigger(0);
        print("Zapper unplugged (a Zapper from input.zapper or an --input script stays)");
        return;
    }
    input_zapper_state aim = {false, 0, 0, false};
    size_t i = 2;
    if (first != "off") {
        auto x = parseCount(args[1]);
        auto y = args.size() > 2 ? parseCount(args[2]) : std::nullopt;
        if (!x || !y || *x > 255 || *y > 239) {
            print(syntax);
            return;
        }
        aim.on_screen = true;
        aim.x = static_cast<int>(*x);
        aim.y = static_cast<int>(*y);
        i = 3;
    }
    bool fire = false;
    unsigned long frames = 2;
    if (i < args.size()) {
        if (lower(args[i]) != "fire") {
            print(syntax);
            return;
        }
        fire = true;
        i++;
        if (i < args.size()) {
            auto n = parseCount(args[i]);
            if (!n) {
                print(syntax);
                return;
            }
            frames = *n;
            i++;
        }
    }
    if (i < args.size()) {
        print(syntax);
        return;
    }
    input_set_zapper(&aim);
    input_zapper_pull_trigger(fire ? cpu_debug_get_cycle() + frames * kFrameCycles : 0);
    std::string where =
        aim.on_screen ? format("pointing at %d,%d", aim.x, aim.y) : std::string("pointing away from the screen");
    print(fire ? format("Zapper: %s, trigger pulled for %lu frame%s (fs or x runs them)", where.c_str(), frames,
                        frames == 1 ? "" : "s")
               : format("Zapper: %s", where.c_str()));
}

void CommandProcessor::cmdSaveState(const Args &args)
{
    const std::string what = (args.size() >= 2) ? lower(args[1]) : "";
    auto slot = (args.size() == 3) ? parseCount(args[2]) : std::optional<unsigned long>(savestate_slot());
    if ((what != "save" && what != "load") || args.size() > 3 || !slot || *slot < 1 || *slot > 9) {
        print("Syntax: savestate save|load [slot]   (slots 1-9; default: the current one)");
        return;
    }
    if (!Emulator::instance().hasRom()) {
        print("No game is loaded");
        return;
    }
    savestate_set_slot(static_cast<int>(*slot));
    if (what == "load") {
        char when[64];
        if (!savestate_slot_time(static_cast<int>(*slot), when, sizeof(when))) {
            print(format("Slot %lu is empty", *slot));
            return;
        }
        savestate_load_slot(); // (the debugger has stopped the game: loaded now, it stays stopped)
        print(format("Loaded slot %lu (saved %s)", *slot, when));
        host.stopped();
        return;
    }
    if (!canRun()) {
        return;
    }
    savestate_request_save_slot(static_cast<int>(*slot)); // taken at the end of this frame's picture...
    pendingSaveSlot = static_cast<int>(*slot);
    Emulator::instance().runFrames(1); // ... on the way to the next vblank
    host.runStarted();
}

std::string CommandProcessor::stopLocation() const
{
    uint16_t pc = cpu_debug_get_pc();
    std::string bank = Disassembler::bankText(pc);
    if (bank.empty()) {
        return "";
    }
    return format(" at %s (%s, ROM offset $%05X)", Disassembler::addressColumn(pc).c_str(), bank.c_str(),
                  static_cast<unsigned>(mapper_debug_prg_offset(pc)));
}

void CommandProcessor::cmdGoScanline(const Args &args)
{
    auto line = (args.size() == 2) ? parseCount(args[1]) : std::nullopt;
    if (!line || *line > 261) {
        print("Syntax: gsl line   (0-261, decimal)");
        return;
    }
    if (!canRun()) {
        return;
    }
    if (Emulator::instance().ended()) {
        reportStop(StopReason::Ended);
        return;
    }
    Emulator::instance().runToScanline(static_cast<unsigned>(*line));
    host.runStarted();
}

// --- breakpoints -------------------------------------------------------------------------------------------------

// "ADDRESS ... [if CONDITION] [do "COMMANDS"]": ARGS becomes the words before 'if' / 'do' (with the command name)
bool CommandProcessor::breakpointExtras(Args &args, BreakpointExtras &extras)
{
    const std::string &raw = rawArguments;
    size_t ifAt = std::string::npos, doAt = std::string::npos;
    char quote = 0;
    for (size_t i = 0; i < raw.size(); i++) {
        char c = raw[i];
        if (quote != 0) {
            quote = (c == quote) ? 0 : quote;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        bool wordStart = (i == 0 || std::isspace(static_cast<unsigned char>(raw[i - 1])));
        bool wordEnd = (i + 2 >= raw.size() || std::isspace(static_cast<unsigned char>(raw[i + 2])));
        if (!wordStart || !wordEnd || i + 2 > raw.size()) {
            continue;
        }
        std::string word = lower(raw.substr(i, 2));
        if (word == "if" && ifAt == std::string::npos && doAt == std::string::npos) {
            ifAt = i;
        } else if (word == "do" && doAt == std::string::npos) {
            doAt = i;
            break; // (the rest is the action)
        }
    }
    size_t baseEnd = std::min(ifAt, doAt);
    auto trim = [](const std::string &text) {
        size_t first = text.find_first_not_of(" \t");
        size_t last = text.find_last_not_of(" \t");
        return (first == std::string::npos) ? std::string() : text.substr(first, last - first + 1);
    };
    if (ifAt != std::string::npos) {
        extras.condition = trim(raw.substr(ifAt + 2, doAt == std::string::npos ? std::string::npos : doAt - ifAt - 2));
        if (extras.condition.empty()) {
            print("'if' needs a condition, e.g. bpx 8000 if a == 0");
            return false;
        }
    }
    if (doAt != std::string::npos) {
        extras.action = trim(raw.substr(doAt + 2));
        if (extras.action.size() >= 2 && (extras.action.front() == '"' || extras.action.front() == '\'') &&
            extras.action.back() == extras.action.front()) {
            extras.action = extras.action.substr(1, extras.action.size() - 2);
        }
        if (extras.action.empty()) {
            print("'do' needs commands, e.g. bpx 8000 do \"d 300; g\"");
            return false;
        }
    }
    if (remote != nullptr && !extras.condition.empty()) {
        if (std::string why = RemoteServer::conditionRefusal(extras.condition); !why.empty()) {
            print(why);
            return false;
        }
    }
    for (size_t start = 0; remote != nullptr && start < extras.action.size();) { // (the action's commands: ';')
        size_t end = std::min(extras.action.find(';', start), extras.action.size());
        std::string command = extras.action.substr(start, end - start);
        std::string why = RemoteServer::refusal(command);
        Args words = split(command);
        std::string name = words.empty() ? std::string() : lower(words[0]);
        if (why.empty() && (name == "load" || name == "bpx" || name == "bpm" || name == "bop")) {
            why = "not over the remote channel: " + name; // (an action runs later, unchecked)
        }
        if (!why.empty()) {
            print(why + " (in a breakpoint's action)");
            return false;
        }
        start = end + 1;
    }
    if (baseEnd != std::string::npos) {
        args = split(args[0] + " " + raw.substr(0, baseEnd));
    }
    return true;
}

void CommandProcessor::addBreakpoint(Breakpoint::Type type, uint16_t address, BreakpointExtras &extras)
{
    if (!extras.condition.empty()) {
        char error[256];
        extras.conditionHandle = script_condition_compile(extras.condition.c_str(), error, sizeof(error));
        if (extras.conditionHandle < 0) {
            print(std::string("Condition: ") + error);
            return;
        }
    }
    if (Emulator::instance().addBreakpoint(type, address, extras.condition, extras.conditionHandle, extras.action) <
        0) {
        print("Cannot set breakpoint: all 16 are in use");
    }
    host.stateChanged();
}

void CommandProcessor::cmdBpx(const Args &words)
{
    Args args = words;
    BreakpointExtras extras;
    if (!breakpointExtras(args, extras)) {
        return;
    }
    if (args.size() != 2) {
        print("Syntax: bpx address [if condition] [do \"commands\"]");
        return;
    }
    auto address = parseAddress(args[1]);
    if (!address) {
        print(unknownAddress(args[1]));
        return;
    }
    addBreakpoint(Breakpoint::Type::Execute, *address, extras);
}

void CommandProcessor::cmdBpm(const Args &words)
{
    Args args = words;
    BreakpointExtras extras;
    if (!breakpointExtras(args, extras)) {
        return;
    }
    if (args.size() < 2 || args.size() > 3) {
        print("Syntax: bpm address [r|w|rw|x]");
        return;
    }
    auto address = parseAddress(args[1]);
    if (!address) {
        print(unknownAddress(args[1]));
        return;
    }
    Breakpoint::Type type = Breakpoint::Type::ReadWrite;
    if (args.size() == 3 && args[1].find('=') == std::string::npos && args[2].find('=') == std::string::npos) {
        std::string verb = lower(args[2]);
        if (verb == "r") {
            type = Breakpoint::Type::Read;
        } else if (verb == "w") {
            type = Breakpoint::Type::Write;
        } else if (verb == "rw") {
            type = Breakpoint::Type::ReadWrite;
        } else if (verb == "x") {
            type = Breakpoint::Type::Execute;
        } else {
            print("Command 'bpm': unrecognized verb '" + args[2] + "'");
            return;
        }
    }
    addBreakpoint(type, *address, extras);
}

void CommandProcessor::cmdBop(const Args &words)
{
    Args args = words;
    BreakpointExtras extras;
    if (!breakpointExtras(args, extras)) {
        return;
    }
    auto opcode = (args.size() == 2) ? parseHex(args[1]) : std::nullopt;
    if (!opcode || *opcode > 0xFF) {
        print("Syntax: bop opcode   (hexadecimal, e.g. bop 9E)");
        return;
    }
    addBreakpoint(Breakpoint::Type::Opcode, static_cast<uint16_t>(*opcode), extras);
}

bool CommandProcessor::breakpointSlots(const Args &args, std::vector<unsigned> &slots)
{
    const auto &bps = Emulator::instance().breakpoints();
    if (args.size() < 2) {
        print("Give breakpoint numbers (see 'bl'), or * for all");
        return false;
    }
    for (size_t i = 1; i < args.size(); i++) {
        if (args[i] == "*") {
            slots.clear();
            for (unsigned s = 0; s < bps.size(); s++) {
                slots.push_back(s);
            }
            return true;
        }
        auto n = parseCount(args[i]);
        if (!n || *n >= bps.size()) {
            print("No breakpoint #" + args[i]);
            return false;
        }
        slots.push_back(static_cast<unsigned>(*n));
    }
    return true;
}

void CommandProcessor::cmdBc(const Args &args)
{
    std::vector<unsigned> slots;
    if (!breakpointSlots(args, slots)) {
        return;
    }
    std::sort(slots.rbegin(), slots.rend()); // highest first, so the numbers of the others stay valid
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    for (unsigned slot : slots) {
        Emulator::instance().removeBreakpoint(slot);
    }
    host.stateChanged();
}

void CommandProcessor::cmdBd(const Args &args)
{
    std::vector<unsigned> slots;
    if (breakpointSlots(args, slots)) {
        for (unsigned slot : slots) {
            Emulator::instance().setBreakpointEnabled(slot, false);
        }
        host.stateChanged();
    }
}

void CommandProcessor::cmdBe(const Args &args)
{
    std::vector<unsigned> slots;
    if (breakpointSlots(args, slots)) {
        for (unsigned slot : slots) {
            Emulator::instance().setBreakpointEnabled(slot, true);
        }
        host.stateChanged();
    }
}

std::string CommandProcessor::breakpointText(unsigned slot) const
{
    const Breakpoint &bp = Emulator::instance().breakpoints()[slot];
    std::string where;
    if (bp.type == Breakpoint::Type::Opcode) {
        where = format("opcode $%02X", bp.address);
    } else if (const std::string *label = symbols.find(bp.address)) {
        where = format("$%04X ", bp.address) + *label;
    } else {
        where = format("$%04X", bp.address);
    }
    static const char *kind[] = {"BPX", "BPM R", "BPM W", "BPM RW", "BOP"};
    std::string text = format("%2u) %-6s %s", slot, kind[static_cast<int>(bp.type)], where.c_str());
    if (!bp.condition.empty()) {
        text += " if " + bp.condition;
    }
    if (!bp.action.empty()) {
        text += " do \"" + bp.action + "\"";
    }
    if (!bp.enabled) {
        text += " [Disabled]";
    }
    return text;
}

void CommandProcessor::cmdBl(const Args &)
{
    const auto &bps = Emulator::instance().breakpoints();
    if (bps.empty()) {
        print("No breakpoints set.");
        return;
    }
    for (unsigned i = 0; i < bps.size(); i++) {
        print(breakpointText(i));
    }
}

// --- memory and registers ----------------------------------------------------------------------------------------

void CommandProcessor::cmdDump(const Args &args)
{
    if (args.size() > 1) {
        auto address = parseAddress(args[1]);
        if (!address) {
            print(unknownAddress(args[1]));
            return;
        }
        dumpAddress = *address;
    } else {
        dumpAddress = static_cast<uint16_t>(dumpAddress + 0x80);
    }
    host.showMemory(dumpAddress);
}

void CommandProcessor::cmdDumpPpu(const Args &args)
{
    if (args.size() > 1) {
        auto address = parseHex(args[1]);
        if (!address || *address > 0x3FFF) {
            print("Invalid PPU address (0000-3FFF)");
            return;
        }
        ppuDumpAddress = static_cast<uint16_t>(*address);
    } else {
        ppuDumpAddress = static_cast<uint16_t>((ppuDumpAddress + 0x80) & 0x3FFF);
    }
    host.showPpuMemory(ppuDumpAddress);
}

void CommandProcessor::cmdUnassemble(const Args &args)
{
    uint16_t address = cpu_debug_get_pc();
    if (args.size() > 1) {
        auto value = parseAddress(args[1]);
        if (!value) {
            print(unknownAddress(args[1]));
            return;
        }
        address = *value;
    }
    host.showCode(address);
}

void CommandProcessor::cmdEditByte(const Args &args)
{
    if (args.size() < 3) {
        print("Syntax: eb address = value [value...]");
        return;
    }
    auto address = parseAddress(args[1]);
    if (!address) {
        print(unknownAddress(args[1]));
        return;
    }
    std::vector<uint8_t> values;
    for (size_t i = 2; i < args.size(); i++) {
        auto value = parseHex(args[i]);
        if (!value || *value > 0xFF) {
            print("Invalid value '" + args[i] + "'");
            return;
        }
        values.push_back(static_cast<uint8_t>(*value));
    }
    uint16_t a = *address;
    for (uint8_t value : values) {
        if (!cpu_debug_write(a, value)) {
            print(format("$%04X: only RAM ($0000-$1FFF, cartridge $6000-$7FFF) can be edited", a));
            break;
        }
        a++;
    }
    host.stateChanged();
}

void CommandProcessor::cmdEditOam(const Args &args)
{
    auto offset = (args.size() >= 3) ? parseHex(args[1]) : std::nullopt;
    if (!offset || *offset > 0xFF) {
        print("Syntax: eo offset = value [value...]   (offset hex 00-FF: sprite n at 4*n)");
        return;
    }
    std::vector<uint8_t> values;
    for (size_t i = 2; i < args.size(); i++) {
        auto value = parseHex(args[i]);
        if (!value || *value > 0xFF) {
            print("Invalid value '" + args[i] + "'");
            return;
        }
        values.push_back(static_cast<uint8_t>(*value));
    }
    unsigned a = static_cast<unsigned>(*offset);
    for (uint8_t value : values) {
        if (a > 0xFF) {
            print("OAM ends at $FF");
            break;
        }
        ppu_debug_write_oam(static_cast<uint8_t>(a++), value);
    }
    host.stateChanged();
}

void CommandProcessor::cmdOamList(const Args &args)
{
    bool all = args.size() == 2 && lower(args[1]) == "all";
    if (args.size() > 2 || (args.size() == 2 && !all)) {
        print("Syntax: oam [all]");
        return;
    }
    OamReport report = decodeOam();
    for (const std::string &line : oamHeader(report)) {
        print(line);
    }
    print(oamColumnTitles());
    for (int i = 0; i < 64; i++) {
        if (all || !report.sprites[static_cast<size_t>(i)].hidden) {
            print(oamRow(report, i));
        }
    }
}

void CommandProcessor::cmdRegister(const Args &args)
{
    if (args.size() == 1) {
        print(format("PC=%04X  A=%02X  X=%02X  Y=%02X  P=%02X  SP=%02X", cpu_debug_get_pc(), cpu_debug_get_reg_a(),
                     cpu_debug_get_reg_x(), cpu_debug_get_reg_y(), cpu_debug_get_reg_p(), cpu_debug_get_sp()));
        return;
    }
    if (args.size() != 3) {
        print("Syntax: r register = value");
        return;
    }
    std::string reg = lower(args[1]);
    auto value =
        (reg == "pc") ? std::optional<unsigned long>(parseAddress(args[2]).value_or(0x10000)) : parseHex(args[2]);
    unsigned long limit = (reg == "pc") ? 0xFFFF : 0xFF;
    if (!value || *value > limit) {
        print("Invalid value '" + args[2] + "'");
        return;
    }
    uint8_t byte = static_cast<uint8_t>(*value);
    if (reg == "pc") {
        cpu_debug_set_pc(static_cast<uint32_t>(*value));
        Emulator::instance().flowTracker().reset(); // the call stack no longer follows what the CPU does
    } else if (reg == "a" || reg == "acc") {
        cpu_debug_set_reg_a(byte);
    } else if (reg == "x") {
        cpu_debug_set_reg_x(byte);
    } else if (reg == "y") {
        cpu_debug_set_reg_y(byte);
    } else if (reg == "p" || reg == "sr") {
        cpu_debug_set_reg_p(byte);
    } else if (reg == "sp" || reg == "s") {
        cpu_debug_set_reg_sp(byte);
        Emulator::instance().flowTracker().reset();
    } else {
        print("Unknown register.  Choices are: pc a x y p sp");
        return;
    }
    host.stateChanged();
}

void CommandProcessor::cmdName(const Args &args)
{
    if (args.size() != 2 && args.size() != 3) {
        print("Syntax: n address name   (n address removes the label)");
        return;
    }
    auto address = parseAddress(args[1]);
    if (!address) {
        print(unknownAddress(args[1]));
        return;
    }
    if (args.size() == 2) {
        if (!symbols.remove(*address)) {
            print(format("$%04X has no label", *address));
            return;
        }
    } else {
        symbols.add(*address, args[2]);
        Symbols::Location where = Symbols::locate(*address);
        if (where.kind == Symbols::Kind::PrgRom) {
            print(format("%s = $%04X: ", args[2].c_str(), *address) + Symbols::describe(where) +
                  (mapper_debug_prg_banked() ? " (this bank only)" : ""));
        }
    }
    symbols.setModified(true);
    host.stateChanged();
}

void CommandProcessor::cmdComment(const Args &args)
{
    if (args.size() < 2) {
        print("Syntax: comment address [text]   (without text: remove the comment)");
        return;
    }
    auto address = parseAddress(args[1]);
    if (!address) {
        print(unknownAddress(args[1]));
        return;
    }
    // the text: the rest of the line as typed (split() would eat '=' and ','); "\n" is a line break, as in .mlb
    std::string text = rawArguments;
    size_t start = text.find_first_not_of(" \t");
    start = (start == std::string::npos) ? text.size() : text.find_first_of(" \t", start);
    start = (start == std::string::npos) ? text.size() : text.find_first_not_of(" \t", start);
    text = (start == std::string::npos) ? std::string() : text.substr(start);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    for (size_t i = 0; (i = text.find("\\n", i)) != std::string::npos; i++) {
        text.replace(i, 2, "\n");
    }
    if (text.empty() && !symbols.commentAt(*address)) {
        print(format("$%04X has no comment", *address));
        return;
    }
    symbols.setComment(*address, text);
    host.stateChanged();
}

void CommandProcessor::cmdLoadBinary(const Args &args)
{
    if (args.size() != 3) {
        print("Syntax: lb file address");
        return;
    }
    auto address = parseAddress(args[2]);
    if (!address) {
        print("Invalid address '" + args[2] + "'");
        return;
    }
    std::ifstream in(args[1], std::ios::binary);
    if (!in) {
        print("Unable to open '" + args[1] + "'");
        return;
    }
    uint16_t a = *address;
    unsigned count = 0;
    char byte;
    while (in.get(byte)) {
        if (!cpu_debug_write(a, static_cast<uint8_t>(byte))) {
            print(format("Stopped at $%04X: only RAM ($0000-$1FFF, cartridge $6000-$7FFF) can be loaded", a));
            break;
        }
        a++;
        count++;
    }
    print(format("Loaded %u bytes at $%04X", count, *address));
    host.stateChanged();
}

void CommandProcessor::cmdEnterCode(const Args &)
{
    host.toggleCodeFocus();
}

void CommandProcessor::cmdOam(const Args &)
{
    host.toggleOamWindow();
}

// rs [on|off]: the game window at a stop shows the frame in progress (drawn so far; the rest -- still the previous
// frame -- dimmed, the beam marked) or the last complete frame. (TODO: Chris wants to revisit the command's name.)
void CommandProcessor::cmdRestoreScreen(const Args &args)
{
    if (args.size() > 2 || (args.size() == 2 && lower(args[1]) != "on" && lower(args[1]) != "off")) {
        print("Syntax: rs [on|off]");
        return;
    }
    host.frameInProgress = (args.size() == 2) ? lower(args[1]) == "on" : !host.frameInProgress;
    if (host.frameInProgress) {
        print(format("Game window: the frame in progress (line %u, dot %u; the dimmed part is still the previous "
                     "frame)",
                     ppu_debug_get_scanline(), ppu_debug_get_scanline_clock()));
    } else {
        print("Game window: the last complete frame");
    }
    host.showGameFrame();
}

void CommandProcessor::cmdLog(const Args &args)
{
    if (args.size() == 1) {
        std::string line = "Message levels:";
        for (int i = 0; i < LOG_CATEGORIES; i++) {
            line += std::string(" ") + log_category_name(static_cast<log_category>(i)) + "=" +
                    log_level_name(static_cast<log_level>(log_levels[i]));
        }
        print(line);
        return;
    }
    // "log debug", "log ppu debug", "log ppu=debug apu=trace" (the command line splits at '=' too: pairs)
    std::string spec;
    for (size_t i = 1; i < args.size(); i++) {
        std::string word = lower(args[i]);
        bool category = log_category_from_name(word.c_str()) >= 0 || word == "all";
        if (category && i + 1 < args.size()) {
            spec += word + "=" + lower(args[i + 1]) + " ";
            i++;
        } else {
            spec += word + " ";
        }
    }
    char error[160];
    if (!log_set_levels(spec.c_str(), error, sizeof(error))) {
        print(std::string("Command 'log': ") + error);
        return;
    }
    cmdLog({"log"});
}

// wbg / wpal: a picture window (viewer_window.hpp) on or off; NAME save FILE: its picture as a PNG (also in
// --batch, where there are no windows)
void CommandProcessor::pictureWindow(ViewerWindow &viewer, const char *name, const char *what, const Args &args)
{
    if (args.size() >= 2 && lower(args[1]) == "save") {
        size_t at = lower(rawArguments).find("save");
        std::string file = fileArgument(rawArguments.substr(at + 4));
        if (file.empty()) {
            print(std::string("Syntax: ") + name + " save file.png");
        } else if (viewer.save(file)) {
            print(std::string(what) + " saved: " + file);
        } else {
            print(std::string(name) + ": cannot write " + file);
        }
        return;
    }
    if (args.size() > 1) {
        print(std::string("Syntax: ") + name + " [save file.png]");
        return;
    }
    if (!host.interactive()) {
        print(std::string(name) + ": no windows in --batch (" + name + " save file.png works)");
        return;
    }
    if (viewer.isOpen()) {
        viewer.close();
        return;
    }
    std::string error;
    if (!viewer.open(error)) {
        print(error);
    }
}

void CommandProcessor::cmdBackground(const Args &args)
{
    pictureWindow(BackgroundViewer::instance(), "wbg", "Background", args);
}

void CommandProcessor::cmdPalette(const Args &args)
{
    pictureWindow(PaletteViewer::instance(), "wpal", "Palette", args);
}

void CommandProcessor::cmdProfilerWindow(const Args &args)
{
    ProfilerWindow::instance().useSymbols(&symbols);
    pictureWindow(ProfilerWindow::instance(), "wprof", "Profiler", args);
}

// prof [on|off|reset|list [sort COLUMN] [asc|desc] [N]]
void CommandProcessor::cmdProfile(const Args &args)
{
    Emulator &emu = Emulator::instance();
    if (!emu.hasRom()) {
        print("No game loaded");
        return;
    }
    Profiler &profiler = emu.profiler();
    const std::string sub = args.size() > 1 ? lower(args[1]) : "list";
    if (sub == "on") {
        if (!profiler.running()) {
            emu.startProfiling();
        }
        print("Profiler on");
    } else if (sub == "off") {
        emu.stopProfiling();
        print("Profiler off (the counts stay: prof list; prof reset forgets them)");
    } else if (sub == "reset") {
        profiler.clear();
        print("Profiler counts cleared");
    } else if (sub == "list") {
        using Column = Profiler::Column;
        static const char *const names[] = {"name", "address", "calls", "incl", "incl%",
                                            "excl", "excl%",   "avg",   "min",  "max"};
        Column column = Column::Excl;
        bool descending = true;
        bool directionGiven = false;
        size_t count = 20;
        for (size_t i = 2; i < args.size(); i++) {
            const std::string word = lower(args[i]);
            if (word == "sort" && i + 1 < args.size()) {
                const std::string name = lower(args[++i]);
                int found = -1;
                for (int c = 0; c < 10; c++) {
                    if (name == names[c]) {
                        found = c;
                    }
                }
                if (found < 0) {
                    print("prof: unknown column " + args[i]);
                    return;
                }
                column = static_cast<Column>(found);
            } else if (word == "asc" || word == "desc") {
                descending = word == "desc";
                directionGiven = true;
            } else if (auto n = parseCount(word)) {
                count = *n;
            } else {
                print("Syntax: prof [on|off|reset|list [sort COLUMN] [asc|desc] [N]]");
                return;
            }
        }
        if (!directionGiven) {
            descending = column != Column::Name && column != Column::Address;
        }
        std::vector<Profiler::Row> rows = profiler.rows(symbols);
        Profiler::sort(rows, column, descending);
        const uint64_t total = profiler.totalCycles();
        const uint64_t frames = profiler.frames();
        print(std::string(profiler.running() ? "Profiling" : "Stopped") +
              format(": %zu routines, %llu frames, %llu cycles", profiler.functions(),
                     static_cast<unsigned long long>(frames), static_cast<unsigned long long>(total)));
        print(ProfilerWindow::header());
        for (size_t i = 0; i < rows.size() && i < count; i++) {
            print(ProfilerWindow::line(rows[i], total, frames, false));
        }
    } else {
        print("Syntax: prof [on|off|reset|list [sort COLUMN] [asc|desc] [N]]");
    }
}

void CommandProcessor::cmdPatterns(const Args &args)
{
    pictureWindow(PatternViewer::instance(), "wpat", "Pattern tables", args);
}

void CommandProcessor::cmdScreenshot(const Args &args)
{
    if (args.size() > 2) {
        print("Syntax: screenshot [file]");
        return;
    }
    char saved[1100];
    if (video_screenshot(args.size() > 1 ? args[1].c_str() : nullptr, saved, sizeof(saved))) {
        print(std::string("Screenshot saved: ") + saved);
    } else {
        print(std::string("Screenshot: cannot write ") + saved);
    }
}

void CommandProcessor::cmdFps(const Args &args)
{
    bool show = !video_fps_shown();
    if (args.size() > 1) {
        std::string value = lower(args[1]);
        if (value == "on") {
            show = true;
        } else if (value == "off") {
            show = false;
        } else {
            print("Syntax: fps [on|off]");
            return;
        }
    }
    video_set_fps(show);
    if (video_is_headless()) {
        print(std::string("FPS counter ") + (show ? "on" : "off") + " (there is no game window to show it in)");
    } else {
        print(std::string("FPS counter ") + (show ? "on (in the game window's title)" : "off"));
    }
}

// lag [on|off|reset]
void CommandProcessor::cmdVersion(const Args &)
{
    print(std::string("ONE Debugger ") + one_version());
}

void CommandProcessor::cmdLag(const Args &args)
{
    const std::string sub = args.size() > 1 ? lower(args[1]) : "";
    if (sub == "on" || sub == "off") {
        video_set_lag_counter(sub == "on");
        print(std::string("Lag counter ") + sub +
              (video_is_headless() ? " (there is no game window to show it in)" : ""));
    } else if (sub == "reset") {
        input_reset_lag_count();
        print("Lag counter reset");
    } else if (args.size() == 1) {
        print(format("Lag frames: %u of %u (last frame: %s)", static_cast<unsigned>(input_lag_count()),
                     static_cast<unsigned>(ppu_debug_get_frame_num()),
                     input_last_frame_lagged() ? "lagged" : "polled"));
    } else {
        print("Syntax: lag [on|off|reset]");
    }
}

void CommandProcessor::cmdReset(const Args &args)
{
    if (!canRun()) {
        return;
    }
    Emulator &emu = Emulator::instance();
    if (args.size() > 1 && lower(args[1]) == "hard") {
        if (!emu.powerCycle()) {
            print("Power on failed");
        }
    } else if (args.size() > 1) {
        print("Command 'reset': unrecognized parameter '" + args[1] + "'");
        return;
    } else if (emu.ended()) {
        print("The emulation has ended: use 'reset hard'");
        return;
    } else {
        emu.softReset();
    }
    host.stopped();
}

void CommandProcessor::cmdQuit(const Args &)
{
    host.quit();
}

void CommandProcessor::cmdHelp(const Args &args)
{
    if (args.size() > 1) {
        std::string name = lower(args[1]);
        for (const Command &command : commands()) {
            if (name == command.name) {
                for (const char *line : command.help) {
                    if (line == nullptr) {
                        break;
                    }
                    print(line);
                }
                return;
            }
        }
        print("No help available for '" + args[1] + "'");
        return;
    }
    std::string names;
    for (const Command &command : commands()) {
        names += command.name;
        names += ' ';
    }
    print("Commands: " + names);
    print(
        "Help on a command: help command.  Keys: F5 go, F8 trace, F10 step, F12 p ret, F6 code window, F7 here, F9 bpx");
}

// --- memory search, freeze, labels, Game Genie --------------------------------------------------------------------

std::string CommandProcessor::addressText(uint32_t address) const
{
    std::string text = format("$%04X", address);
    if (address <= 0xFFFF) {
        if (const std::string *label = symbols.find(static_cast<uint16_t>(address))) {
            text += " " + *label;
        }
    }
    return text;
}

// ---- s: SoftICE's S command ----

void CommandProcessor::cmdFind(const Args &)
{
    // tokens: words, and quoted strings ('...' or "..."); blanks and commas separate
    struct Token {
        std::string text;
        bool quoted;
    };
    std::vector<Token> tokens;
    const std::string &line = rawArguments;
    for (size_t i = 0; i < line.size();) {
        char c = line[i];
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
            i++;
        } else if (c == '\'' || c == '"') {
            size_t close = line.find(c, i + 1);
            if (close == std::string::npos) {
                print("Missing closing quote");
                return;
            }
            tokens.push_back({line.substr(i + 1, close - i - 1), true});
            i = close + 1;
        } else {
            size_t stop = line.find_first_of(" \t,'\"", i);
            if (stop == std::string::npos) {
                stop = line.size();
            }
            tokens.push_back({line.substr(i, stop - i), false});
            i = stop;
        }
    }

    bool anyCase = false, ppu = false;
    size_t t = 0;
    for (; t < tokens.size() && !tokens[t].quoted && tokens[t].text.size() > 1 && tokens[t].text[0] == '-'; t++) {
        for (char flag : lower(tokens[t].text.substr(1))) {
            if (flag == 'c') {
                anyCase = true;
            } else if (flag == 'p') {
                ppu = true;
            } else if (flag == 'u') {
                print("-u (Unicode) is not supported: NES games have no Unicode text");
                return;
            } else {
                print(std::string("Unknown option -") + flag + " (options: -c ignore case, -p PPU memory)");
                return;
            }
        }
    }
    const char *syntax = "Syntax: s [-cp] [address L length data-list]";
    if (t == tokens.size()) {
        if (t > 0) {
            print(syntax);
        } else if (!find.active) {
            print("No previous search.  " + std::string(syntax));
        } else {
            findNext();
        }
        return;
    }

    // address
    if (tokens[t].quoted) {
        print(syntax);
        return;
    }
    uint32_t limit = ppu ? 0x4000 : 0x10000;
    std::optional<unsigned long> start;
    if (ppu) {
        start = parseHex(tokens[t].text);
    } else if (auto a = parseAddress(tokens[t].text)) {
        start = *a;
    }
    if (!start || *start >= limit) {
        print(ppu ? "Invalid PPU address (0000-3FFF)" : unknownAddress(tokens[t].text));
        return;
    }
    t++;

    // L length (hex, as in SoftICE): "l 100" or "l100"
    std::optional<unsigned long> length;
    if (t < tokens.size() && !tokens[t].quoted && lower(tokens[t].text)[0] == 'l') {
        if (tokens[t].text.size() > 1) {
            length = parseHex(tokens[t].text.substr(1));
            t++;
        } else if (t + 1 < tokens.size() && !tokens[t + 1].quoted) {
            length = parseHex(tokens[t + 1].text);
            t += 2;
        }
    }
    if (!length || *length == 0) {
        print("Missing or invalid length.  " + std::string(syntax));
        return;
    }
    const unsigned long first = *start, count = *length; // both checked above

    // data-list
    std::vector<uint8_t> pattern;
    std::vector<bool> fold;
    for (; t < tokens.size(); t++) {
        if (tokens[t].quoted) {
            if (tokens[t].text.empty()) {
                print("Empty string");
                return;
            }
            for (char c : tokens[t].text) {
                pattern.push_back(static_cast<uint8_t>(c));
                fold.push_back(anyCase && std::isalpha(static_cast<unsigned char>(c)));
            }
        } else {
            auto value = parseHex(tokens[t].text);
            if (!value || *value > 0xFF) {
                print("Invalid byte '" + tokens[t].text + "' (data-list: bytes 00-FF and quoted strings)");
                return;
            }
            pattern.push_back(static_cast<uint8_t>(*value));
            fold.push_back(false);
        }
    }
    if (pattern.empty()) {
        print("Missing data-list.  " + std::string(syntax));
        return;
    }

    find.active = true;
    find.ppu = ppu;
    find.pattern = pattern;
    find.anyCase = fold;
    find.next = static_cast<uint32_t>(first);
    find.end = static_cast<uint32_t>(std::min<unsigned long>(first + count, limit));
    findNext();
}

void CommandProcessor::findNext()
{
    auto read = [this](uint32_t a) {
        return find.ppu ? ppu_debug_read(static_cast<uint16_t>(a)) : cpu_debug_read(static_cast<uint16_t>(a));
    };
    const size_t n = find.pattern.size();
    for (uint32_t a = find.next; a + n <= find.end; a++) {
        size_t i = 0;
        for (; i < n; i++) {
            uint8_t have = read(a + static_cast<uint32_t>(i));
            uint8_t want = find.pattern[i];
            if (find.anyCase[i]) {
                have = static_cast<uint8_t>(std::tolower(have));
                want = static_cast<uint8_t>(std::tolower(want));
            }
            if (have != want) {
                break;
            }
        }
        if (i == n) {
            find.next = a + 1;
            if (find.ppu) {
                print(format("Pattern found at PPU $%04X", a));
                ppuDumpAddress = static_cast<uint16_t>(a);
                host.showPpuMemory(ppuDumpAddress, static_cast<uint32_t>(n));
            } else {
                print("Pattern found at " + addressText(a));
                dumpAddress = static_cast<uint16_t>(a);
                host.showMemory(dumpAddress, static_cast<uint32_t>(n));
            }
            return;
        }
    }
    find.next = find.end;
    print("Pattern not found");
}

void CommandProcessor::searchList(size_t from)
{
    MemorySearch &search = Emulator::instance().search();
    const size_t page = 16;
    const bool cpu = search.target() == MemorySearch::Target::Cpu;
    for (const MemorySearch::Candidate &c : search.list(from, page)) {
        std::string where = (search.target() == MemorySearch::Target::Oam) ? format("OAM $%02X", c.address)
                            : cpu                                          ? addressText(c.address)
                                                                           : format("PPU $%04X", c.address);
        std::string line = format("  %-22s %02X (was %02X)", where.c_str(), c.current, c.previous);
        if (cpu && c.lastWriter >= 0) {
            line += "  written by " + addressText(static_cast<uint32_t>(c.lastWriter));
        }
        print(line);
    }
    if (search.count() > from + page) {
        print(format("  ... %zu more: cs list %zu", search.count() - from - page, from + page));
    }
}

void CommandProcessor::cmdSearch(const Args &)
{
    MemorySearch &search = Emulator::instance().search();
    std::string rest = rawArguments;
    rest.erase(0, rest.find_first_not_of(" \t"));
    size_t lastChar = rest.find_last_not_of(" \t");
    rest.erase(lastChar == std::string::npos ? 0 : lastChar + 1);
    std::istringstream words(rest);
    std::string verb, arg, extra;
    words >> verb >> arg >> extra;
    verb = lower(verb);

    if (verb == "start") {
        MemorySearch::Target target = MemorySearch::Target::Cpu;
        std::string t = lower(arg);
        if (t == "ppu") {
            target = MemorySearch::Target::Ppu;
        } else if (t == "oam") {
            target = MemorySearch::Target::Oam;
        } else if (!t.empty() && t != "cpu") {
            print("Syntax: cs start [cpu|ppu|oam]");
            return;
        }
        search.start(target);
        print(format("Searching %s: %zu bytes.  Let the game run, then filter (help cs).",
                     search.describeTarget().c_str(), search.count()));
        return;
    }
    if (!search.active()) {
        print("No search in progress: start one with 'cs start [cpu|ppu|oam]'");
        return;
    }
    if (verb.empty()) {
        print(format("Searching %s: %zu candidates, %s compares%s", search.describeTarget().c_str(), search.count(),
                     search.isSigned() ? "signed" : "unsigned",
                     search.undoDepth() ? format(", %zu filters to undo", search.undoDepth()).c_str() : ""));
        return;
    }
    if (verb == "stop") {
        search.stop();
        print("Search ended");
        return;
    }
    if (verb == "reset") {
        search.start(search.target());
        print(format("Search restarted on %s: %zu bytes", search.describeTarget().c_str(), search.count()));
        return;
    }
    if (verb == "list") {
        auto from = arg.empty() ? std::optional<unsigned long>(0) : parseCount(arg);
        if (!from) {
            print("Syntax: cs list [first]");
            return;
        }
        if (search.count() == 0) {
            print("No candidates left (cs undo goes back a step)");
            return;
        }
        searchList(*from);
        return;
    }
    if (verb == "undo") {
        print(search.undo() ? format("Back to %zu candidates", search.count()) : "Nothing to undo");
        return;
    }
    if (verb == "signed" || verb == "unsigned") {
        search.setSigned(verb == "signed");
        print(std::string("Comparisons are ") + verb + " from now on");
        return;
    }

    // filters
    size_t left = 0;
    auto needValue = [&](const std::string &text, int &value) {
        auto v = parseHex(text);
        if (!v || *v > 0xFF) {
            print("Expected a byte value (hexadecimal), not '" + text + "'");
            return false;
        }
        value = static_cast<int>(*v);
        return true;
    };
    struct OpWord {
        const char *word;
        MemorySearch::Op op;
    };
    static const OpWord kWords[] = {
        {"same", MemorySearch::Op::Same}, {"diff", MemorySearch::Op::Changed}, {"changed", MemorySearch::Op::Changed},
        {"up", MemorySearch::Op::Up},     {"down", MemorySearch::Op::Down},
    };
    bool done = false;
    for (const OpWord &w : kWords) {
        if (verb == w.word && arg.empty()) {
            left = search.filterValue(w.op);
            done = true;
        }
    }
    if (!done && (verb == "written" || verb == "notwritten" || verb == "read" || verb == "notread")) {
        if (search.target() != MemorySearch::Target::Cpu) {
            print("Access filters only work for 'cs start cpu'");
            return;
        }
        left = search.filterAccess(verb.find("written") != std::string::npos, verb.rfind("not", 0) != 0);
        done = true;
    }
    if (!done) {
        // operators: = v, == v, != v, < v, > v, +n, -n (with or without spaces)
        static const struct {
            const char *symbol;
            MemorySearch::Op op;
        } kOps[] = {
            {"!=", MemorySearch::Op::NotEqual}, {"==", MemorySearch::Op::Equal},  {"=", MemorySearch::Op::Equal},
            {"<", MemorySearch::Op::Less},      {">", MemorySearch::Op::Greater}, {"+", MemorySearch::Op::Plus},
            {"-", MemorySearch::Op::Minus},
        };
        for (const auto &o : kOps) {
            size_t n = std::strlen(o.symbol);
            if (rest.compare(0, n, o.symbol) == 0) {
                std::string valueText = rest.substr(n);
                valueText.erase(0, valueText.find_first_not_of(" \t"));
                int value;
                if (valueText.empty() || valueText.find_first_of(" \t") != std::string::npos ||
                    !needValue(valueText, value)) {
                    if (valueText.empty()) {
                        print(std::string("Syntax: cs ") + o.symbol + " value");
                    }
                    return;
                }
                left = search.filterValue(o.op, value);
                done = true;
                break;
            }
        }
    }
    if (!done) {
        print("Unknown search filter '" + rest + "' (help cs)");
        return;
    }
    print(format("%zu candidate%s left", left, left == 1 ? "" : "s"));
    if (left > 0 && left <= 8) {
        searchList(0);
    }
}

void CommandProcessor::cmdFreeze(const Args &args)
{
    Emulator &emu = Emulator::instance();
    if (args.size() == 1) {
        if (emu.frozen().empty()) {
            print("Nothing is frozen.");
        }
        for (const Emulator::Freeze &f : emu.frozen()) {
            print(format("  %-24s = %02X", addressText(f.address).c_str(), f.value));
        }
        return;
    }
    if (args.size() > 3) {
        print("Syntax: freeze [address [value]]");
        return;
    }
    auto address = parseAddress(args[1]);
    if (!address) {
        print(unknownAddress(args[1]));
        return;
    }
    uint8_t value = cpu_debug_read(*address);
    if (args.size() == 3 && args[1].find('=') == std::string::npos && args[2].find('=') == std::string::npos) {
        auto v = parseHex(args[2]);
        if (!v || *v > 0xFF) {
            print("Invalid value '" + args[2] + "'");
            return;
        }
        value = static_cast<uint8_t>(*v);
    }
    if (!emu.freeze(*address, value)) {
        print(format("$%04X is not RAM: only $0000-$1FFF and cartridge RAM $6000-$7FFF can be frozen", *address));
        return;
    }
    print(format("%s frozen at %02X", addressText(*address).c_str(), value));
    host.stateChanged();
}

void CommandProcessor::cmdUnfreeze(const Args &args)
{
    Emulator &emu = Emulator::instance();
    if (args.size() != 2) {
        print("Syntax: unfreeze address | *");
        return;
    }
    if (args[1] == "*") {
        emu.unfreezeAll();
        return;
    }
    auto address = parseAddress(args[1]);
    if (!address || !emu.unfreeze(*address)) {
        print("'" + args[1] + "' is not frozen");
    }
}

void CommandProcessor::cmdLabelSave(const Args &args)
{
    std::string file = (args.size() > 1) ? args[1] : symbols.fileName();
    if (file.empty()) {
        print("Syntax: lsave file");
        return;
    }
    if (!symbols.save(file)) {
        print("Cannot write '" + file + "'");
        return;
    }
    symbols.setFileName(file);
    symbols.setModified(false);
    std::string comments = symbols.comments() == 0
                               ? std::string()
                               : format(" and %zu comment%s", symbols.comments(), symbols.comments() == 1 ? "" : "s");
    print(format("%zu label%s", symbols.size(), symbols.size() == 1 ? "" : "s") + comments + " saved to " + file);
}


namespace {
// The mapper's PRG bank size in bytes, 0 when it does not switch PRG banks (or no game)
uint32_t prgBankBytes()
{
    size_t size = 0;
    if (mapper_debug_prg_rom(&size) == nullptr || size == 0) {
        return 0;
    }
    for (uint32_t at = 0x8000; at <= 0xFFFF; at += 0x2000) {
        int bank = 0, banks = 0;
        if (mapper_debug_prg_bank(static_cast<uint16_t>(at), &bank, &banks) && banks > 0) {
            return static_cast<uint32_t>(size / static_cast<size_t>(banks));
        }
    }
    return 0;
}

// "07:E177 ryu_hurt+3": the address (with its PRG bank when banked) and the label at or before it
std::string codeAt(const Symbols &symbols, uint16_t address, int32_t prgOffset)
{
    const uint32_t bankSize = prgBankBytes();
    char text[16];
    if (prgOffset >= 0 && bankSize != 0) {
        std::snprintf(text, sizeof(text), "%02X:%04X", static_cast<unsigned>(prgOffset) / bankSize, address);
    } else {
        std::snprintf(text, sizeof(text), "%04X", address);
    }
    std::string name;
    if (prgOffset >= 0) {
        name = symbols.describeNear({Symbols::Kind::PrgRom, static_cast<uint32_t>(prgOffset)});
    } else if (const std::string *label = symbols.find(address)) {
        name = *label;
    }
    return name.empty() ? std::string(text) : std::string(text) + " " + name;
}

const char *jumpName(uint8_t opcode)
{
    if (opcode == 0x4C) {
        return "JMP";
    }
    if (opcode == 0x6C) {
        return "JMP (ind)";
    }
    if (opcode == 0x60 || opcode == 0x40) {
        return opcode == 0x60 ? "RTS" : "RTI";
    }
    return mnemonic_strings[instruction_info[opcode].type];
}
} // namespace

void CommandProcessor::cmdBacktrace(const Args &args)
{
    Emulator &emu = Emulator::instance();
    const FlowTracker &flow = emu.flowTracker();
    if (!emu.hasRom()) {
        print("No game loaded");
        return;
    }
    if (!emu.flowTracking()) {
        emu.startFlowTracking();
        print("(call tracking starts now; calls made earlier are guessed from the stack)");
    }
    if (args.size() > 1 && lower(args[1]) == "jumps") {
        if (flow.transfers().empty()) {
            print("No jumps or taken branches recorded yet");
        }
        for (auto it = flow.transfers().rbegin(); it != flow.transfers().rend(); ++it) {
            print(std::string(jumpName(it->opcode)) + "  " + codeAt(symbols, it->from, it->fromOffset) + "  ->  " +
                  codeAt(symbols, it->to, it->toOffset));
        }
        return;
    }
    const uint16_t pc = cpu_debug_get_pc();
    std::string line = "#0  " + codeAt(symbols, pc, mapper_debug_prg_offset(pc));
    if (const Transfer *by = flow.reachedBy(pc)) {
        line += "    reached by " + std::string(jumpName(by->opcode)) + " from " +
                codeAt(symbols, by->from, by->fromOffset);
    }
    print(line);
    int n = 1;
    const std::vector<CallFrame> &frames = flow.frames();
    for (auto it = frames.rbegin(); it != frames.rend(); ++it, ++n) {
        std::string what;
        switch (it->kind) {
        case CallFrame::Kind::Call:
            what = "JSR " + codeAt(symbols, it->target, it->targetOffset) + "    returns to " +
                   format("%04X", it->returnAddress);
            break;
        case CallFrame::Kind::Nmi:
            what = "NMI: interrupted here (handler " + codeAt(symbols, it->target, it->targetOffset) + ")";
            break;
        case CallFrame::Kind::Irq:
            what = "IRQ: interrupted here (handler " + codeAt(symbols, it->target, it->targetOffset) + ")";
            break;
        case CallFrame::Kind::Brk: what = "BRK (handler " + codeAt(symbols, it->target, it->targetOffset) + ")"; break;
        case CallFrame::Kind::Reset: what = "RESET"; break;
        }
        print(format("#%d  ", n) + codeAt(symbols, it->callSite, it->callSiteOffset) + "    " + what);
    }
    {
        // Frames older than the tracking (it starts at the first bt, or after a state load or reset): look at the
        // stack above the recorded ones for return addresses that follow a JSR
        const uint8_t sp = cpu_debug_get_sp();
        int guessed = 0;
        const unsigned from =
            frames.empty() ? static_cast<unsigned>(sp) + 1 : static_cast<unsigned>(frames.front().entrySp) + 3;
        for (unsigned at = from; at < 0xFF; at++) {
            const uint16_t ret = static_cast<uint16_t>((cpu_debug_read(static_cast<uint16_t>(0x100 + at)) |
                                                        (cpu_debug_read(static_cast<uint16_t>(0x101 + at)) << 8)) +
                                                       1);
            if (ret >= 3 && cpu_debug_read(static_cast<uint16_t>(ret - 3)) == 0x20) {
                const uint16_t site = static_cast<uint16_t>(ret - 3);
                const uint16_t target = cpu_debug_read_word(static_cast<uint16_t>(site + 1));
                print(format("#%d  ", n++) + codeAt(symbols, site, mapper_debug_prg_offset(site)) + "    JSR " +
                      codeAt(symbols, target, mapper_debug_prg_offset(target)) + "    (guessed)");
                guessed++;
                at++; // (the word was used)
            }
        }
        if (guessed == 0 && frames.empty()) {
            print("(no calls on the stack)");
        }
    }
}

void CommandProcessor::cmdCdl(const Args &args)
{
    Emulator &emu = Emulator::instance();
    CodeDataLog &cdl = emu.codeLog();
    if (!emu.hasRom()) {
        print("No game loaded");
        return;
    }
    size_t prgSize = 0;
    mapper_debug_prg_rom(&prgSize);
    const uint32_t bankSize = prgBankBytes();
    const std::string sub = args.size() > 1 ? lower(args[1]) : "";
    auto percent = [](size_t part, size_t whole) {
        return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
    };

    if (sub == "on" || sub == "off") {
        std::string saved;
        if (sub == "off" && cdl.enabled()) { // (what was learned is kept: game.cdl is read again next time)
            const std::string file = romSibling(emu.romPath(), ".cdl");
            const std::string error = cdl.save(file);
            saved = error.empty() ? ", saved to " + file : ", cannot save: " + error;
        }
        cdl.setEnabled(sub == "on");
        if (sub == "on") {
            emu.startFlowTracking(); // (jump targets and subroutine entries come from the flow tracker)
        }
        print(std::string("Code/data log ") + sub + saved);
    } else if (sub == "clear") {
        cdl.clear();
        print("Code/data log cleared");
    } else if (sub == "stats" || sub.empty()) {
        const CodeDataLog::Counts all = cdl.count(0, prgSize);
        print(format("Code/data log %s: %zu bytes of PRG-ROM: %.1f%% code, %.1f%% data, %.1f%% unknown",
                     cdl.enabled() ? "on" : "off", all.bytes, percent(all.code, all.bytes),
                     percent(all.data, all.bytes), percent(all.unknown, all.bytes)));
        if (all.pcm != 0) {
            print(format("  %zu of the data bytes are DMC samples", all.pcm));
        }
        if (sub == "stats" && bankSize != 0) {
            for (size_t first = 0, bank = 0; first < prgSize; first += bankSize, bank++) {
                const CodeDataLog::Counts c = cdl.count(first, first + bankSize);
                print(format("  bank %zu: %zu code, %zu data, %zu unknown (%.1f%% known)", bank, c.code, c.data,
                             c.unknown, percent(c.code + c.data, c.bytes)));
            }
        }
    } else if (sub == "unseen") {
        size_t first = 0, last = prgSize;
        if (args.size() > 2 && bankSize != 0) {
            const long bank = std::strtol(args[2].c_str(), nullptr, 10);
            if (bank < 0 || static_cast<size_t>(bank) * bankSize >= prgSize) {
                print(format("bank: 0-%zu", prgSize / bankSize - 1));
                return;
            }
            first = static_cast<size_t>(bank) * bankSize;
            last = first + bankSize;
        }
        const size_t minimum = args.size() > 3 ? static_cast<size_t>(std::strtoul(args[3].c_str(), nullptr, 10)) : 16;
        const auto ranges = cdl.unseen(first, last, minimum == 0 ? 1 : minimum);
        int shown = 0;
        for (const auto &range : ranges) {
            if (++shown > 60) {
                print(format("... %zu more ranges", ranges.size() - 60));
                break;
            }
            print(format("PRG $%05zX-$%05zX  %zu bytes%s", range.first, range.second - 1, range.second - range.first,
                         bankSize != 0 ? format("  (bank %zu)", range.first / bankSize).c_str() : ""));
        }
        if (ranges.empty()) {
            print("No unseen ranges that long");
        }
    } else if (sub == "mark" || sub == "unmark") {
        const auto address = args.size() > 2 ? parseAddress(args[2]) : std::nullopt;
        const auto length = args.size() > 3 ? parseHex(args[3]) : std::optional<unsigned long>(1);
        const int32_t offset = address ? mapper_debug_prg_offset(*address) : -1;
        if (!address || !length || *length == 0 || offset < 0) {
            print("Syntax: cdl mark|unmark address [length]   (PRG-ROM address, length in hex, 1 by default)");
            return;
        }
        const size_t changed = cdl.markData(static_cast<size_t>(offset), *length, sub == "mark");
        print(format("%zu byte%s %s", changed, changed == 1 ? "" : "s",
                     sub == "mark" ? "marked as data (the code window shows them as .db rows)"
                                   : "no longer marked as data"));
        host.stateChanged();
    } else if (sub == "save") {
        const std::string file = args.size() > 2 ? args[2] : romSibling(emu.romPath(), ".cdl");
        const std::string error = cdl.save(file);
        print(error.empty() ? "Code/data log saved to " + file : "Cannot save: " + error);
    } else if (sub == "load") {
        const bool force = args.size() > 1 && lower(args.back()) == "force";
        const std::string file =
            args.size() > 2 && lower(args[2]) != "force" ? args[2] : romSibling(emu.romPath(), ".cdl");
        const std::string error = cdl.load(file, force);
        if (error.empty()) {
            const CodeDataLog::Counts all = cdl.count(0, prgSize);
            print(format("Code/data log loaded from %s: %.1f%% code, %.1f%% data", file.c_str(),
                         percent(all.code, all.bytes), percent(all.data, all.bytes)));
        } else {
            print("Cannot load: " + error);
        }
    } else {
        print(
            "Syntax: cdl [on|off|clear|stats|unseen [bank [bytes]]|mark|unmark address [length]|save [file]|load [file] [force]]");
    }
}

void CommandProcessor::cmdLabelLoad(const Args &args)
{
    if (args.size() != 2) {
        print("Syntax: lload file");
        return;
    }
    std::string kind;
    int count = symbols.loadFile(args[1], &kind);
    if (count < 0) {
        print("Cannot read '" + args[1] + "'");
        return;
    }
    // a Mesen file becomes the one lsave writes (unless that would leave changes behind in another one)
    const bool mlb = args[1].size() > 4 && lower(args[1].substr(args[1].size() - 4)) == ".mlb";
    if (mlb && (symbols.fileName().empty() || !symbols.modified())) {
        symbols.setFileName(args[1]);
    }
    print(format("%d label%s loaded from ", count, count == 1 ? "" : "s") + args[1] + " (" + kind + ")");
    host.stateChanged();
}

// gg on|off|delete|list|enable|disable: the game's codes, which the core applies (cheats.h). In the UI the list is
// remembered per game (cheats_file()); --batch keeps it for the run only.
void CommandProcessor::ggApply(const Args &args)
{
    const std::string sub = lower(args[1]);
    auto describe = [this](const cheat_entry &e) {
        char text[9];
        cheats_encode(&e.code, text);
        std::string line =
            format("%s %-8s %s = %02X", e.on ? "[x]" : "[ ]", text, addressText(e.code.address).c_str(), e.code.value);
        if (e.code.has_compare) {
            line += format(" if it reads %02X", e.code.compare);
        }
        if (e.description[0] != '\0') {
            line += std::string("  ") + e.description;
        }
        return line;
    };
    if (sub == "list") {
        print(format("Game Genie: %s, %d of %d code%s ticked", cheats_enabled() ? "on" : "off (gg enable)",
                     cheats_active_count(), cheats_count(), cheats_count() == 1 ? "" : "s") +
              (cheats_file()[0] != '\0' ? std::string(" (") + cheats_file() + ")" : std::string()));
        for (int i = 0; i < cheats_count(); i++) {
            print("  " + describe(*cheats_get(i)));
        }
        return;
    }
    if (sub == "db") { // the known codes for this game (data/cheats/README.md)
        if (cheats_db_count() == 0) {
            print("Game Genie: no known codes for this game (gg on CODE adds one of your own)");
            return;
        }
        const bool number = (args.size() == 3) && std::isdigit(static_cast<unsigned char>(args[2][0]));
        const unsigned long pick = number ? std::strtoul(args[2].c_str(), nullptr, 10) : 0;
        if (pick >= 1) {
            int added = cheats_db_add(static_cast<int>(pick) - 1);
            if (added == 0) {
                print(format("Game Genie: no known code %lu (gg db lists them)", pick));
                return;
            }
            print(format("Game Genie: %d code%s ticked (gg list)", added, added == 1 ? "" : "s"));
            host.stateChanged();
            return;
        }
        std::string filter; // gg db WORD...: the ones whose description has it
        for (size_t i = 2; i < args.size(); i++) {
            filter += (i > 2 ? " " : "") + lower(args[i]);
        }
        print(format("Known codes for %s: %d (gg db N ticks one)", cheats_db_game(), cheats_db_count()));
        for (int i = 0; i < cheats_db_count(); i++) {
            char codes[128], description[CHEATS_DESC_SIZE];
            cheats_db_get(i, codes, sizeof(codes), description, sizeof(description));
            if (filter.empty() || lower(description).find(filter) != std::string::npos) {
                print(format("%4d  %-17s %s", i + 1, codes, description));
            }
        }
        return;
    }
    if (sub == "enable" || sub == "disable") {
        cheats_set_enabled(sub == "enable");
        print(format("Game Genie %s (%d code%s ticked)", sub == "enable" ? "on" : "off", cheats_active_count(),
                     cheats_active_count() == 1 ? "" : "s"));
        host.stateChanged();
        return;
    }
    if (args.size() < 3 || (sub != "on" && args.size() != 3)) {
        print("Syntax: gg on code [description] | gg off code|* | gg delete code|*");
        return;
    }
    if (args[2] == "*" && (sub == "off" || sub == "delete")) {
        if (sub == "delete") {
            cheats_clear();
        } else {
            for (int i = 0; i < cheats_count(); i++) {
                cheats_set_on(i, false);
            }
        }
        print(sub == "delete" ? "Game Genie: all codes deleted" : "Game Genie: all codes unticked");
        host.stateChanged();
        return;
    }
    cheat_code code;
    if (!cheats_decode(args[2].c_str(), &code)) {
        print("Not a Game Genie code: 6 or 8 of the letters A P Z L G I T Y E O X U K S V N");
        return;
    }
    if (sub == "on") {
        std::string description;
        for (size_t i = 3; i < args.size(); i++) {
            description += (i > 3 ? " " : "") + args[i];
        }
        if (!cheats_add(&code, description.c_str())) {
            print(format("Game Genie: no room for more codes (%d at most)", CHEATS_MAX));
            return;
        }
        print("Game Genie: " + describe(*cheats_get(cheats_find(&code))));
        if (code.has_compare && mapper_debug_read_prg_rom(code.address) != code.compare) {
            print(format("   (the ROM there now reads %02X: the code works only while the bank with %02X is in)",
                         mapper_debug_read_prg_rom(code.address), code.compare));
        }
        if (cheats_active_count() > CHEATS_GAME_GENIE_MAX) {
            print(format("   (%d codes ticked: the real Game Genie takes %d)", cheats_active_count(),
                         CHEATS_GAME_GENIE_MAX));
        }
    } else {
        int i = cheats_find(&code);
        if (i < 0) {
            print("Game Genie: that code is not in the list (gg list)");
            return;
        }
        if (sub == "off") {
            cheats_set_on(i, false);
            print("Game Genie: " + describe(*cheats_get(i)));
        } else {
            cheats_delete(i);
            print("Game Genie: deleted");
        }
    }
    host.stateChanged();
}

void CommandProcessor::cmdGameGenie(const Args &typedArgs)
{
    Args args = typedArgs;
    if (args.size() >= 2) {
        std::string sub = lower(args[1]);
        if (sub == "apply") { // (the words people try: gg apply CODE = gg on CODE, gg decode CODE = gg CODE)
            args[1] = sub = "on";
        } else if (sub == "decode" && args.size() == 3) {
            args.erase(args.begin() + 1);
        }
        if (sub == "on" || sub == "off" || sub == "delete" || sub == "list" || sub == "db" || sub == "enable" ||
            sub == "disable") {
            ggApply(args);
            return;
        }
    }
    if (args.size() == 2) {
        auto code = gameGenieDecode(args[1]);
        if (!code) {
            print("Not a Game Genie code: 6 or 8 of the letters A P Z L G I T Y E O X U K S V N");
            return;
        }
        uint8_t now = mapper_debug_read_prg_rom(code->address); // (the cartridge's byte, not a code's)
        std::string typed;                                      // the code as typed, without separators
        for (char c : args[1]) {
            if (c != '-' && c != ' ') {
                typed += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
        }
        std::string text = format("%s: %s = %02X", typed.c_str(), addressText(code->address).c_str(), code->value);
        if (code->compare) {
            text += format(" if it reads %02X", *code->compare);
        }
        text +=
            format("   (ROM there now: %02X%s)", now,
                   !code->compare ? "" : (now == *code->compare ? ", compare matches" : ", compare does not match"));
        print(text);
        // The third letter's top bit only says how long the code is (set in 8-letter codes); the Game Genie ignores it
        // when it applies the code, so a code with it the other way round does the same thing.
        if (std::string standard = gameGenieEncode(*code); standard != typed) {
            print("   (usually written " + standard + ": the 3rd letter's top bit marks the code's length, " +
                  "and is not part of the address)");
        }
        return;
    }
    if (args.size() == 3 || args.size() == 4) {
        auto address = parseAddress(args[1]);
        auto value = parseHex(args[2]);
        std::optional<unsigned long> compare;
        if (args.size() == 4) {
            compare = parseHex(args[3]);
        }
        if (!address) {
            for (size_t i = 1; i < args.size(); i++) { // (a code where an address was expected: gg CODE apply)
                if (gameGenieDecode(args[i])) {
                    print("'" + args[i] + "' is a Game Genie code: gg " + args[i] + " decodes it, gg on " + args[i] +
                          " applies it");
                    return;
                }
            }
            print(unknownAddress(args[1]) + " (gg address value [compare] makes a code)");
            return;
        }
        if (*address < 0x8000) {
            print("Game Genie codes patch ROM: the address must be $8000-$FFFF");
            return;
        }
        if (!value || *value > 0xFF || (args.size() == 4 && (!compare || *compare > 0xFF))) {
            print("Values are bytes (hexadecimal)");
            return;
        }
        GameGenieCode code{*address, static_cast<uint8_t>(*value), std::nullopt};
        if (compare) {
            code.compare = static_cast<uint8_t>(*compare);
        }
        print(format("%s = %02X%s: ", addressText(*address).c_str(), code.value,
                     compare ? format(" if %02X", *code.compare).c_str() : "") +
              gameGenieEncode(code));
        return;
    }
    print("Syntax: gg code | gg address value [compare] | gg on code [description] | gg off|delete code|* | gg list | "
          "gg enable|disable");
}

// --- script commands ('!' prefix, from -s scripts) ------------------------------------------------------------------

void CommandProcessor::scriptCommand(const Args &args)
{
    auto fail = [this](const std::string &what) {
        failedAssertions++;
        print("ASSERTION FAILED: " + what);
    };
    if (args.size() == 4 && lower(args[0]) == "assert" && lower(args[1]) == "reg") {
        std::string reg = lower(args[2]);
        auto expected = parseHex(args[3]);
        if (!expected) {
            print("Invalid value '" + args[3] + "'");
            return;
        }
        unsigned long actual;
        if (reg == "a")
            actual = cpu_debug_get_reg_a();
        else if (reg == "x")
            actual = cpu_debug_get_reg_x();
        else if (reg == "y")
            actual = cpu_debug_get_reg_y();
        else if (reg == "p")
            actual = cpu_debug_get_reg_p();
        else if (reg == "sp")
            actual = cpu_debug_get_sp();
        else if (reg == "pc")
            actual = cpu_debug_get_pc();
        else {
            print("Unknown register '" + args[2] + "'");
            return;
        }
        if (actual != *expected) {
            fail(format("%s = $%lX, expected $%lX", reg.c_str(), actual, *expected));
        }
    } else if (args.size() == 4 && lower(args[0]) == "assert" && lower(args[1]) == "mem") {
        auto address = parseAddress(args[2]);
        auto expected = parseHex(args[3]);
        if (!address || !expected) {
            print("Syntax: !assert mem address value");
            return;
        }
        uint8_t actual = cpu_debug_read(*address);
        if (actual != *expected) {
            fail(format("[$%04X] = $%02X, expected $%02lX", *address, actual, *expected));
        }
    } else if (args.size() == 4 && lower(args[0]) == "assert" && lower(args[1]) == "previous") {
        // the code window's step back: the instruction it shows before ADDR
        auto address = parseAddress(args[2]);
        auto expected = parseAddress(args[3]);
        if (!address || !expected) {
            print("Syntax: !assert previous address expected");
            return;
        }
        uint16_t actual = Disassembler(symbols).previous(*address);
        if (actual != *expected) {
            fail(format("instruction before $%04X found at $%04X, expected $%04X", *address, actual, *expected));
        }
    } else if (args.size() == 5 && lower(args[0]) == "assert" && lower(args[1]) == "backtrack") {
        // where the code window starts when ADDR (the PC) needs LINES instructions above it
        auto address = parseAddress(args[2]);
        auto lines = parseCount(args[3]);
        auto expected = parseAddress(args[4]);
        if (!address || !lines || !expected) {
            print("Syntax: !assert backtrack address lines expected");
            return;
        }
        uint16_t actual = Disassembler(symbols).backtrack(*address, static_cast<int>(*lines));
        if (actual != *expected) {
            fail(format("code shown before $%04X starts at $%04X, expected $%04X", *address, actual, *expected));
        }
    } else if (args.size() >= 3 && lower(args[0]) == "assert" && lower(args[1]) == "comment") {
        // the comment at an address, line breaks as \n ("-" for none), compared word by word
        auto address = parseAddress(args[2]);
        if (!address) {
            print("Syntax: !assert comment address text|-");
            return;
        }
        std::string expected;
        for (size_t i = 3; i < args.size(); i++) {
            expected += (i > 3 ? " " : "") + args[i];
        }
        const std::string *comment = symbols.commentAt(*address);
        std::string actual = comment ? *comment : "-";
        for (size_t i = 0; (i = actual.find('\n', i)) != std::string::npos; i += 2) {
            actual.replace(i, 1, "\\n");
        }
        std::string words; // compared word by word (script arguments lose '=' and ',', as everywhere)
        for (const std::string &word : split(actual)) {
            words += (words.empty() ? "" : " ") + word;
        }
        if (words != (expected.empty() ? "-" : expected)) {
            fail(format("comment at $%04X is '%s', expected '%s'", *address, actual.c_str(), expected.c_str()));
        }
    } else if (args.size() >= 4 && lower(args[0]) == "assert" && lower(args[1]) == "oam") {
        // the decoded sprite's row (as in the OAM window) has these words, in this order
        auto index = parseCount(args[2]);
        if (!index || *index > 63) {
            fail("no sprite " + args[2]);
            return;
        }
        auto words = [](std::string text) {
            for (char &c : text) {
                c = (c == ',' || c == '=' || c == ';' || c == ':') ? ' ' : c;
            }
            return split(text);
        };
        std::vector<std::string> expected(args.begin() + 3, args.end());
        std::string row = oamRow(decodeOam(), static_cast<int>(*index));
        std::vector<std::string> actual = words(row);
        bool found = false;
        for (size_t i = 0; i + expected.size() <= actual.size() && !found; i++) {
            found = std::equal(expected.begin(), expected.end(), actual.begin() + static_cast<long>(i),
                               [](const std::string &a, const std::string &b) { return lower(a) == lower(b); });
        }
        if (!found) {
            std::string want;
            for (const std::string &w : expected) {
                want += (want.empty() ? "" : " ") + w;
            }
            fail("sprite " + args[2] + " is '" + row + "', expected '" + want + "' in it");
        }
    } else if (args.size() >= 4 && lower(args[0]) == "assert" && lower(args[1]) == "watch") {
        // the value a watch shows, as in the watch window ("$1F 31")
        auto index = parseCount(args[2]);
        std::string expected;
        for (size_t i = 3; i < args.size(); i++) {
            expected += (i > 3 ? " " : "") + args[i];
        }
        const std::vector<Watch> &list = Watches::instance().list();
        if (!index || *index >= list.size()) {
            fail("no watch " + args[2]);
            return;
        }
        std::string actual = list[*index].valueText(symbols);
        if (actual != expected) {
            fail("watch " + args[2] + " shows '" + actual + "', expected '" + expected + "'");
        }
    } else if (args.size() == 3 && lower(args[0]) == "assert" &&
               (lower(args[1]) == "frame" || lower(args[1]) == "scanline")) {
        auto expected = parseCount(args[2]); // (decimal, as fs and gsl take them)
        unsigned long actual = (lower(args[1]) == "frame") ? ppu_debug_get_frame_num() : ppu_debug_get_scanline();
        if (!expected || actual != *expected) {
            fail(format("%s = %lu, expected %s", lower(args[1]).c_str(), actual, args[2].c_str()));
        }
    } else if (args.size() == 4 && lower(args[0]) == "assert" && lower(args[1]) == "pad") {
        uint8_t expected = 0;
        int port = (args[2] == "2") ? 1 : 0;
        if (!input_parse_buttons(lower(args[3]).c_str(), &expected)) {
            print("Buttons: a b select start up down left right, joined by +, or -");
            return;
        }
        if (input_buttons(port) != expected) {
            fail(format("pad %d = $%02X, expected %s ($%02X)", port + 1, input_buttons(port), args[3].c_str(),
                        expected));
        }
    } else if (args.size() == 4 && lower(args[0]) == "assert" && lower(args[1]) == "cdl") {
        // the code/data log at a PRG-ROM offset: !assert cdl code|data|jump|sub|unseen OFFSET[,no]
        auto offset = parseHex(args[3]);
        if (!offset) {
            print("Syntax: !assert cdl code|data|jump|sub|unseen offset");
            return;
        }
        const uint8_t flags = Emulator::instance().codeLog().flagsAt(static_cast<int32_t>(*offset));
        const std::string what = lower(args[2]);
        const uint8_t wanted = what == "code"   ? CodeDataLog::Code
                               : what == "data" ? CodeDataLog::Data
                               : what == "jump" ? CodeDataLog::JumpTarget
                               : what == "sub"  ? CodeDataLog::SubEntryPoint
                                                : 0;
        if (what == "unseen" ? flags != 0 : (wanted == 0 || (flags & wanted) == 0)) {
            fail(format("cdl flags at PRG $%lX are $%02X, expected %s", *offset, flags, what.c_str()));
        }
    } else if (args.size() == 4 && lower(args[0]) == "assert" && lower(args[1]) == "cdlpcm") {
        // DMC sample bytes recorded in the whole PRG-ROM: at least N (!assert cdlpcm min N)
        size_t prgSize = 0;
        mapper_debug_prg_rom(&prgSize);
        auto minimum = parseCount(args[3]);
        const size_t pcm = Emulator::instance().codeLog().count(0, prgSize).pcm;
        if (lower(args[2]) != "min" || !minimum || pcm < *minimum) {
            fail(format("%zu DMC sample bytes in the log, expected at least %s", pcm, args[3].c_str()));
        }
    } else if (args.size() >= 3 && lower(args[0]) == "assert" && lower(args[1]) == "bt") {
        // the call stack: !assert bt depth N | !assert bt site N ADDRESS | !assert bt target N ADDRESS (N = 1: innermost)
        if (!Emulator::instance().flowTracking()) {
            fail("call tracking is off until bt (or cdl on) is first used");
            return;
        }
        const std::vector<CallFrame> &frames = Emulator::instance().flowTracker().frames();
        const std::string what = lower(args[2]);
        if (what == "depth" && args.size() == 4) {
            auto expected = parseCount(args[3]);
            if (!expected || frames.size() != *expected) {
                fail(format("call stack depth is %zu, expected %s", frames.size(), args[3].c_str()));
            }
        } else if ((what == "site" || what == "target") && args.size() == 5) {
            auto n = parseCount(args[3]);
            auto expected = parseAddress(args[4]);
            if (!n || !expected || *n < 1 || *n > frames.size()) {
                fail(format("no call frame %s (depth %zu)", args[3].c_str(), frames.size()));
                return;
            }
            const CallFrame &frame = frames[frames.size() - *n];
            const uint16_t actual = what == "site" ? frame.callSite : frame.target;
            if (actual != *expected) {
                fail(format("call frame %lu %s is $%04X, expected $%04X", *n, what.c_str(), actual, *expected));
            }
        } else {
            print("Syntax: !assert bt depth N | !assert bt site N address | !assert bt target N address");
        }
    } else if (args.size() >= 3 && lower(args[0]) == "assert" && lower(args[1]) == "lag") {
        // !assert lag N | !assert lag min N | !assert lag last 0|1 (the last frame lagged)
        const bool atLeast = lower(args[2]) == "min" && args.size() == 4;
        const bool last = lower(args[2]) == "last" && args.size() == 4;
        auto expected = parseCount(args.back());
        if (!expected || (!atLeast && !last && args.size() != 3)) {
            print("Syntax: !assert lag N | !assert lag min N | !assert lag last 0|1");
        } else if (last) {
            if (input_last_frame_lagged() != (*expected != 0)) {
                fail(format("the last frame %s", input_last_frame_lagged() ? "lagged" : "did not lag"));
            }
        } else if (atLeast ? input_lag_count() < *expected : input_lag_count() != *expected) {
            fail(format("%u lag frames, expected %s%s", static_cast<unsigned>(input_lag_count()),
                        atLeast ? "at least " : "", args.back().c_str()));
        }
    } else if (args.size() >= 3 && lower(args[0]) == "assert" && lower(args[1]) == "prof") {
        // !assert prof calls|incl|excl|min|max ADDRESS N | !assert prof routines N | !assert prof balanced
        const Profiler &profiler = Emulator::instance().profiler();
        const std::string what = lower(args[2]);
        std::vector<Profiler::Row> rows = profiler.rows(symbols);
        if (what == "routines" && args.size() == 4) {
            auto expected = parseCount(args[3]);
            if (!expected || rows.size() - 1 != *expected) {
                fail(format("%zu profiled routines, expected %s", rows.size() - 1, args[3].c_str()));
            }
        } else if (what == "leaf" && args.size() == 4) { // the routine called nothing and was not interrupted
            auto address = parseAddress(args[3]);
            const Profiler::Row *found = nullptr;
            for (const Profiler::Row &row : rows) {
                if (!row.isRoot && address && row.cpuAddress == *address) {
                    found = &row;
                }
            }
            if (found == nullptr || found->incl != found->excl) {
                fail("Incl and Excl of " + args[3] + " differ (or it is not profiled)");
            }
        } else if (what == "balanced" && args.size() == 3) { // every cycle is in exactly one row's Excl
            uint64_t sum = 0;
            for (const Profiler::Row &row : rows) {
                sum += row.excl;
            }
            if (sum != profiler.totalCycles()) {
                fail(format("Excl adds up to %llu, total is %llu", static_cast<unsigned long long>(sum),
                            static_cast<unsigned long long>(profiler.totalCycles())));
            }
        } else if (args.size() == 5) {
            auto address = parseAddress(args[3]);
            auto expected = parseCount(args[4]);
            const Profiler::Row *found = nullptr;
            for (const Profiler::Row &row : rows) {
                if (!row.isRoot && address && row.cpuAddress == *address) {
                    found = &row;
                }
            }
            if (what == "root" && expected) { // !assert prof root _ N
                found = &rows.back();
            }
            if (found == nullptr || !expected) {
                fail("no profiled routine at " + args[3]);
                return;
            }
            uint64_t actual = 0;
            if (what == "calls") {
                actual = found->calls;
            } else if (what == "incl" || what == "root") {
                actual = found->incl;
            } else if (what == "excl") {
                actual = found->excl;
            } else if (what == "min") {
                actual = found->min;
            } else if (what == "max") {
                actual = found->max;
            } else {
                print("Syntax: !assert prof calls|incl|excl|min|max address N | root _ N | routines N | balanced");
                return;
            }
            if (actual != *expected) {
                fail(format("%s of %s is %llu, expected %s", what.c_str(), args[3].c_str(),
                            static_cast<unsigned long long>(actual), args[4].c_str()));
            }
        } else {
            print("Syntax: !assert prof calls|incl|excl|min|max address N | root _ N | routines N | balanced");
        }
    } else if (args.size() == 4 && lower(args[0]) == "set" && lower(args[1]) == "reg") {
        cmdRegister({"r", args[2], args[3]});
    } else {
        print(
            "Script commands: !assert reg r value, !assert mem address value, !assert previous address expected, !assert backtrack address lines expected, !assert watch n value, !assert comment address text, !assert oam n text, !assert frame n, !assert scanline n, !assert pad 1|2 buttons, !assert cdl code|data|jump|sub|unseen offset, !assert cdlpcm min n, !assert bt depth|site|target ..., !assert prof calls|incl|excl|min|max|routines|balanced ..., !assert lag [min] n | lag last 0|1, !set reg r value");
    }
}

// --- watches -----------------------------------------------------------------------------------------------------

void CommandProcessor::cmdWatch(const Args &args)
{
    Watches &watches = Watches::instance();
    if (args.size() == 1) {
        if (watches.empty()) {
            print("No watches.  Syntax: watch [-p] address [b|w|s|bin|l n] [name]");
        }
        for (size_t i = 0; i < watches.list().size(); i++) {
            const Watch &w = watches.list()[i];
            print(format("  %2zu %-12s %-6s %-5s = %s", i, w.label(symbols).c_str(), w.addressText().c_str(),
                         w.typeText().c_str(), w.valueText(symbols).c_str()));
        }
        return;
    }
    const char *syntax = "Syntax: watch [-p] address [b|w|s|bin|l n] [name]";
    Watch watch;
    size_t i = 1;
    if (lower(args[i]) == "-p") {
        watch.ppu = true;
        i++;
    }
    if (i >= args.size()) {
        print(syntax);
        return;
    }
    std::optional<uint16_t> address;
    if (watch.ppu) {
        auto value = parseHex(args[i]);
        if (value && *value <= 0x3FFF) {
            address = static_cast<uint16_t>(*value);
        }
    } else {
        address = parseAddress(args[i]);
    }
    if (!address) {
        print(watch.ppu ? "Invalid PPU address '" + args[i] + "' ($0000-$3FFF)" : unknownAddress(args[i]));
        return;
    }
    watch.address = *address;
    i++;
    if (i < args.size()) {
        std::string type = lower(args[i]);
        bool known = true;
        if (type == "b") {
            watch.type = Watch::Type::Byte;
        } else if (type == "w") {
            watch.type = Watch::Type::Word;
            watch.length = 2;
        } else if (type == "s") {
            watch.type = Watch::Type::Signed;
        } else if (type == "bin") {
            watch.type = Watch::Type::Binary;
        } else if (type[0] == 'l' && (type.size() == 1 || std::isdigit(static_cast<unsigned char>(type[1])))) {
            std::string count = type.size() > 1 ? type.substr(1) : (i + 1 < args.size() ? args[++i] : "");
            auto n = parseCount(count);
            if (!n || *n < 1 || *n > 16) {
                print("Invalid length '" + count + "' (1-16 bytes)");
                return;
            }
            watch.type = Watch::Type::Bytes;
            watch.length = static_cast<int>(*n);
        } else {
            known = false; // a name
        }
        if (known) {
            i++;
        }
    }
    if (i < args.size()) {
        watch.name = args[i++];
    }
    if (i < args.size()) {
        print(syntax);
        return;
    }
    for (size_t n = 0; n < watches.list().size(); n++) {
        const Watch &w = watches.list()[n];
        if (w.address == watch.address && w.ppu == watch.ppu && w.type == watch.type && w.length == watch.length) {
            if (!loadingWatches) {
                print(format("Already watched (watch %zu)", n));
            }
            return;
        }
    }
    if (!watches.add(watch)) {
        print(format("Cannot add a watch: all %zu are in use", Watches::kMax));
        return;
    }
    if (loadingWatches) {
        return;
    }
    std::string label = watch.label(symbols);
    print(format("Watch %zu: %s%s %s = %s", watches.list().size() - 1, label.empty() ? "" : (label + " ").c_str(),
                 watch.addressText().c_str(), watch.typeText().c_str(), watch.valueText(symbols).c_str()));
    if (!watches.save()) {
        print("Cannot write " + watches.file());
    }
    host.watchesChanged(true);
}

void CommandProcessor::cmdWatchClear(const Args &args)
{
    Watches &watches = Watches::instance();
    if (args.size() < 2) {
        print("Syntax: wc number... | *");
        return;
    }
    std::vector<size_t> remove;
    for (size_t i = 1; i < args.size(); i++) {
        if (args[i] == "*") {
            for (size_t n = 0; n < watches.list().size(); n++) {
                remove.push_back(n);
            }
            continue;
        }
        auto n = parseCount(args[i]);
        if (!n || *n >= watches.list().size()) {
            print("No watch '" + args[i] + "' (watch lists them)");
            return;
        }
        remove.push_back(*n);
    }
    std::sort(remove.begin(), remove.end());
    remove.erase(std::unique(remove.begin(), remove.end()), remove.end());
    for (auto it = remove.rbegin(); it != remove.rend(); ++it) {
        watches.remove(*it);
    }
    if (!watches.save()) {
        print("Cannot write " + watches.file());
    }
    host.watchesChanged(false);
}

void CommandProcessor::cmdWatchWindow(const Args &)
{
    host.toggleWatchWindow();
}

void CommandProcessor::loadWatches()
{
    const std::string &file = Watches::instance().file();
    std::ifstream in(file);
    if (file.empty() || !in) {
        return;
    }
    loadingWatches = true;
    for (std::string line; std::getline(in, line);) {
        Args args = split(line);
        if (!args.empty() && lower(args[0]) == "watch" && args.size() > 1) {
            cmdWatch(args);
        }
    }
    loadingWatches = false;
    size_t count = Watches::instance().list().size();
    if (count > 0) {
        print(std::to_string(count) + (count == 1 ? " watch" : " watches") + " from " + file);
        host.watchesChanged(true);
    }
}
