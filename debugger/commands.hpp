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
// The debugger's command language (SoftICE style). Shared by the ImGui UI and the --batch mode.
#pragma once
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>
#include "emulator.hpp"
#include "symbols.hpp"

// What the commands need from whoever is hosting them (the UI, or batch mode).
class CommandHost {
public:
    virtual ~CommandHost() = default;
    virtual void print(const std::string &line) = 0;
    virtual void quit() = 0;
    virtual void stateChanged() {}             // registers/memory may have changed: refresh views
    virtual void stopped() { stateChanged(); } // the emulation stopped (step, breakpoint...): refresh views
    // show memory at an address; mark > 0 highlights that many bytes there (a match of s)
    virtual void showMemory(uint16_t /*address*/, uint32_t /*mark*/ = 0) {}
    virtual void showPpuMemory(uint16_t /*address*/, uint32_t /*mark*/ = 0) {}
    virtual void showCode(uint16_t /*address*/) {}
    virtual void toggleOamWindow() {}
    virtual void toggleWatchWindow() {}            // ww
    virtual void watchesChanged(bool /*added*/) {} // added: show the watch window; the list empty: close it
    virtual void toggleCodeFocus() {}              // ec: keyboard to the code window and back
    // Address of the code window's cursor line, if the code window has the keyboard (EC); for 'here'
    virtual std::optional<uint16_t> codeCursor() { return std::nullopt; }
    // rs: the game window shows the frame in progress at each stop (else the last complete frame)
    bool frameInProgress = false;
    virtual void showGameFrame() {} // show it now, as frameInProgress says
    virtual void runStarted() {}    // x/g/gsl armed a background run
    // The UI (not --batch): a game's .watch and .init files are used, and loaded games go into the recent list
    virtual bool interactive() const { return false; }
    virtual bool confirmDiscardLabels() { return true; } // load: unsaved labels -- save them, or drop them, or cancel
    virtual void romLoaded() {}                          // load: a (new) game is on (windows, menus)
};

class CommandProcessor {
public:
    // The latest CommandProcessor is the one Lua's debugger.command() talks to
    CommandProcessor(CommandHost &host, Symbols &symbols);
    ~CommandProcessor();
    CommandProcessor(const CommandProcessor &) = delete;
    CommandProcessor &operator=(const CommandProcessor &) = delete;

    void execute(const std::string &line); // one command line ('#' comment, '!' script command)
    // Run LINE and return what it printed (Lua's debugger.command), lines separated by '\n'
    std::string executeCaptured(const std::string &line);
    // The remote channel (remote.hpp): TEE also gets what is printed (null: no longer); while REMOTE is set, the
    // commands run are checked against its allowlist (breakpoint conditions and actions, load's directories too)
    void setTee(std::string *tee) { this->tee = tee; }
    void setRemote(const class RemoteServer *remote) { this->remote = remote; }
    void notice(const std::string &line) { print(line); } // a line in the command window (the agent's messages)
    const Symbols &labels() const { return symbols; }

    // Command lines waiting to run (the -s file, source, breakpoint actions): the host runs the next one whenever
    // the emulation is stopped, so a 'g' among them runs until the next stop before the line after it.
    void enqueue(const std::vector<std::string> &lines, bool atFront = false);
    bool hasPending() const { return !pending.empty(); }
    std::string takePending();
    void clearPending() { pending.clear(); }
    bool nextNeedsRom() const; // the next waiting command cannot run before a game is loaded

    // load: power on another game (its labels, watches and init file come with it)
    bool loadRom(const std::string &file);
    // The code/data log is kept in game.cdl (Mesen's format): read when a game starts (if the file exists; returns the
    // message to show, "" if there was nothing to load), written by "cdl off" and, silently, when quitting or
    // loading another game while the log is on.
    static std::string loadSavedCodeLog();
    static void saveCodeLogIfOn();
    static std::string romSibling(const std::string &rom, const char *extension); // game.nes -> game.mlb
    static bool initFiles; // run game.init when a game is loaded (not with --batch, --no-init)
    // SoftICE-style help line for a partly typed command: the matching commands, then (once the name is complete)
    // what it does, then (after a space) its syntax. Empty for an empty line.
    std::string hint(const std::string &partial) const;
    // Tab: the command line completed as far as it is unambiguous. The word being typed is completed against the
    // command names (first word, or after "help"), the cs subcommands, or cs start's memories; one match gets a
    // space after it, several are completed to the part they all share. Unchanged if nothing matches.
    std::string complete(const std::string &partial) const;
    void reportStop(StopReason reason); // print why the emulation stopped
    void loadWatches();                 // the watch list's file (Watches::file()), if there is one
    bool assertionFailed() const { return failedAssertions > 0; }

    // Used by the UI's function keys as well
    void stepInto(unsigned count = 1);
    void stepOver();
    void stepToReturn();
    void go();

    std::optional<uint16_t> parseAddress(const std::string &text) const; // $hex, hex or a label
    std::string unknownAddress(const std::string &text) const;           // why parseAddress failed

private:
    using Args = std::vector<std::string>;
    struct Command {
        const char *name;
        void (CommandProcessor::*run)(const Args &);
        const char *help[7]; // help lines, nullptr-terminated
    };
    static const std::vector<Command> &commands();
    void scriptCommand(const Args &args);

    void cmdTrace(const Args &);
    void cmdStep(const Args &);
    void cmdGo(const Args &);
    void cmdExecute(const Args &);
    void cmdGoScanline(const Args &);
    void cmdFrameStep(const Args &);
    void cmdPress(const Args &);
    void cmdZapper(const Args &);
    void cmdSaveState(const Args &);
    std::string stopLocation() const; // " at 05:BAB7 (PRG bank 5 of 8, ROM offset $17AB7)" in banked ROM, else ""
    int pendingSaveSlot = 0;          // savestate save: the slot being saved by the fs that follows
    void cmdHere(const Args &);
    void cmdBpx(const Args &);
    void cmdBpm(const Args &);
    void cmdBop(const Args &);
    void cmdBc(const Args &);
    void cmdBd(const Args &);
    void cmdBe(const Args &);
    void cmdBl(const Args &);
    void cmdDump(const Args &);
    void cmdDumpPpu(const Args &);
    void cmdUnassemble(const Args &);
    void cmdEditByte(const Args &);
    void cmdEditOam(const Args &);
    void cmdOamList(const Args &);
    void cmdRegister(const Args &);
    void cmdName(const Args &);
    void cmdComment(const Args &);
    void cmdLoadBinary(const Args &);
    void cmdOam(const Args &);
    void cmdBackground(const Args &);
    void cmdPalette(const Args &);
    void cmdProfilerWindow(const Args &);
    void cmdProfile(const Args &);
    void cmdPatterns(const Args &);
    void pictureWindow(class ViewerWindow &viewer, const char *name, const char *what, const Args &args); // wbg, wpal
    void cmdSearch(const Args &);
    void cmdFind(const Args &);
    void findNext();
    void cmdFreeze(const Args &);
    void cmdUnfreeze(const Args &);
    void cmdLabelSave(const Args &);
    void cmdLabelLoad(const Args &);
    void cmdBacktrace(const Args &);
    void cmdCdl(const Args &);
    void cmdGameGenie(const Args &);
    void ggApply(const Args &); // gg on|off|delete|list|enable|disable
    void cmdWatch(const Args &);
    void cmdWatchClear(const Args &);
    void cmdWatchWindow(const Args &);
    void searchList(size_t from);
    std::string addressText(uint32_t address) const; // $C000 or $C000 label
    void cmdEnterCode(const Args &);
    void cmdRestoreScreen(const Args &);
    void cmdScreenshot(const Args &);
    void cmdLog(const Args &);
    void cmdLua(const Args &);
    void cmdSource(const Args &);
    void cmdLoad(const Args &);
    void cmdFps(const Args &);
    void cmdLag(const Args &);
    void cmdVersion(const Args &);
    void cmdReset(const Args &);
    void cmdQuit(const Args &);
    void cmdHelp(const Args &);

    // bpx/bpm/bop: the arguments before ' if ' / ' do ', the condition (compiled: its handle) and the action
    struct BreakpointExtras {
        std::string condition, action;
        int conditionHandle = -1;
    };
    bool breakpointExtras(Args &args, BreakpointExtras &extras);
    void addBreakpoint(Breakpoint::Type type, uint16_t address, BreakpointExtras &extras);
    bool breakpointSlots(const Args &args, std::vector<unsigned> &slots);
    bool canRun(); // false (and says why) inside a Lua callback, where the emulation cannot be resumed
    void print(const std::string &line); // to the host, or to the capture of executeCaptured()
    std::string breakpointText(unsigned slot) const;

    CommandHost &host;
    Symbols &symbols;
    uint16_t dumpAddress = 0x0000;
    uint16_t ppuDumpAddress = 0x0000;
    int failedAssertions = 0;
    bool loadingWatches = false; // loadWatches(): quietly, without saving
    struct Find {                // s: the last pattern, so that 's' alone continues after the last match
        bool active = false;
        bool ppu = false;
        std::vector<uint8_t> pattern;
        std::vector<bool> anyCase; // -c: this byte is a letter of a string, compare without case
        uint32_t next = 0;         // where the next search starts
        uint32_t end = 0;          // one past the last address searched
    } find;
    std::string rawArguments; // the command line after the command name (cs has its own syntax)
    std::deque<std::string> pending;
    std::string *capture = nullptr; // executeCaptured(): print() appends here
    std::string *tee = nullptr;     // setTee()
    const class RemoteServer *remote = nullptr;
};
