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
// The ImGui user interface of one-debugger: a window of its own next to the game window, laid out like the Turbo
// Vision one (../app.hpp) on a grid of characters -- menu bar, windows 1-8, status line -- with the same menus,
// keys and theme colors. The emulation runs from the main loop (run()), as Turbo Vision's idle() runs it.
#pragma once
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "../commands.hpp"
#include "dialogs.hpp"
#include "views.hpp"

struct SDL_Window;
struct SDL_Renderer;
union SDL_Event;
struct GameKey;

class DebuggerApp : public CommandHost {
public:
    // startupMessages: shown in the command window (e.g. problems in the config file)
    DebuggerApp(Symbols &symbols, std::deque<std::string> script, const std::vector<std::string> &startupMessages);
    ~DebuggerApp() override;
    DebuggerApp(const DebuggerApp &) = delete;
    DebuggerApp &operator=(const DebuggerApp &) = delete;

    const std::string &startError() const { return error; } // the window could not be opened: why
    void run();                                             // until the debugger quits
    void shutDown();                                        // the window and ImGui
    int exitCode() const;

    // CommandHost
    void print(const std::string &line) override;
    void quit() override;
    void stateChanged() override;
    void stopped() override;
    void showMemory(uint16_t address, uint32_t mark = 0) override;
    void showPpuMemory(uint16_t address, uint32_t mark = 0) override;
    void showCode(uint16_t address) override;
    void toggleOamWindow() override;
    void toggleWatchWindow() override;
    void watchesChanged(bool added) override;
    void toggleCodeFocus() override;
    std::optional<uint16_t> codeCursor() override;
    void showGameFrame() override;
    void runStarted() override;
    bool interactive() const override { return true; }
    bool confirmDiscardLabels() override { return confirmQuit(); }
    void romLoaded() override;

    // Commands of the menus, the status line and the keys
    enum : int {
        cmGo = 200,
        cmTrace,
        cmStepOver,
        cmStepReturn,
        cmBreak,
        cmToggleBreakpoint,
        cmShowScreen,
        cmHelpCommands,
        cmReset,
        cmPowerCycle,
        cmWinCpuMemory,
        cmWinPpuMemory,
        cmWinOam,
        cmWinCode,
        cmWinRegisters,
        cmWinPpu,
        cmWinCommand,
        cmDefaultLayout,
        cmEnterCode,
        cmHere,
        cmScreenshot,
        cmFps,
        cmWinWatch,
        cmLuaRun,
        cmLuaStop,
        cmLuaList,
        cmOpenRom,
        cmQuit,
        cmNext,
        cmZoom,
        cmResize,
        cmTile,
        cmCascade,
        cmFontLarger,
        cmFontSmaller,
        cmQuickReference,
        cmRecentRom, // cmRecentRom + n: the n-th recent game (0-8)
        cmRecentRomLast = cmRecentRom + 8,
    };

private:
    // A rectangle of character cells on the desktop (the screen between the menu bar and the status line), as
    // Turbo Vision's TRect: a = top left, b = bottom right (exclusive)
    struct Cells {
        int ax = 0, ay = 0, bx = 0, by = 0;
        bool operator==(const Cells &o) const { return ax == o.ax && ay == o.ay && bx == o.bx && by == o.by; }
        bool operator!=(const Cells &o) const { return !(*this == o); }
    };
    // The debugger's windows, by their numbers (shown in their title bars, as Turbo Vision does)
    enum PaneId { kNoPane = 0, kRegisters, kPpu, kCpuMemory, kPpuMemory, kCode, kCommand, kOam, kWatch, kPanes };
    struct Pane {
        const char *title = "";
        bool open = false;
        std::unique_ptr<View> view;
        Cells place;                      // where to put it at the next frame (placePending)
        bool placePending = false;        // (the user moves and sizes the windows; ImGui keeps where they are)
        float x = 0, y = 0, w = 0, h = 0; // where it is (pixels, at the last frame)
        bool zoomed = false;
        Cells beforeZoom;
    };
    struct MenuItemDef {
        std::string label; // "~O~pen game...": O is its key in the open menu; "" = a separator
        std::string key;   // "F3"
        int command = 0;
    };
    struct MenuDef {
        std::string title; // "~F~ile": Alt+F opens it
        std::vector<MenuItemDef> items;
    };

    // window, ImGui, frames
    bool openWindow();
    void setStyle();
    void updateMetrics();
    void frame(Dialog *dialog); // one frame: input, then everything drawn (and DIALOG on top, if not null)
    void runDialog(Dialog &dialog);
    void capture(); // ONE_DEBUGGER_CAPTURE
    static bool eventHook(void *sdlEvent, void *context);
    bool sdlEvent(SDL_Event &event);
    void idle(); // the emulation, the queued commands, the game window: as Turbo Vision's idle()

    // the desktop and its windows
    Cells desktop() const;
    Cells cellsOf(const Pane &pane) const;
    void place(PaneId id, const Cells &where);
    void openPane(PaneId id, const Cells &where, std::unique_ptr<View> view);
    void closePane(PaneId id);
    void select(PaneId id) { focusRequest = id; }
    PaneId focused() const; // the window with the keyboard (one asked for this frame counts already)
    View *focusedView();
    void drawPane(PaneId id);
    void drawDesktop();
    Pane &pane(PaneId id) { return panes[static_cast<size_t>(id)]; }

    void createWindows();
    void defaultLayout();
    void closeGameWindows();
    void openCpuMemory();
    void openPpuMemory();
    void openOam();
    void openWatches();
    void openCode();
    void openRegisters();
    void openPpu();
    void openCommand();
    int leftColumnWidth() const;
    Cells sideArea() const;
    Cells codeArea() const;
    Cells watchArea() const;
    void nextWindow();
    void zoomWindow();
    void tileWindows();
    void cascadeWindows();
    void changeFontSize(int delta);

    // menus, status line, keys
    std::vector<MenuDef> menus() const;
    void drawMenuBar();
    void drawMenuBox(const MenuDef &menu, int x);
    void drawStatusLine();
    void handleInput();
    void menuInput();
    void sizeInput();
    void openMenu(int index);
    void closeMenu();
    void dispatchKey(const KeyPress &key, bool commandLineFirst);
    void paste();        // Ctrl+V, Shift+Insert: the clipboard onto the command line
    void copy();         // Ctrl+C, Ctrl+Insert: the command window's selection to the clipboard
    void pastePrimary(); // the middle button: the X11 primary selection onto the command line
    void defer(std::function<void()> action) { deferred.push_back(std::move(action)); }

    // commands
    void doCommand(int command);
    void toggleBreakpointAtCursor();
    bool confirmQuit();
    void gameKey(const GameKey &key);
    void runLuaScript();
    void openRomDialog();
    std::string chooseFile(const std::string &dir, const char *wildcard, const char *title);

    Symbols &symbols;
    Disassembler disasm;
    CommandProcessor commands;
    std::deque<std::string> notices; // log messages (log.h), shown by idle()
    uint32_t loadedStates = 0;       // savestate_load_count() when the windows were last refreshed
    int afterStop = 0;               // F8 / F10 / F12 pressed while the game ran: the step to take once it has stopped
    std::string hint;                // the help line (status line) for the command being typed

    std::array<Pane, kPanes> panes{};
    RegistersView *regView = nullptr;
    PpuView *ppuView = nullptr;
    MemoryView *memView = nullptr, *ppuMemView = nullptr;
    OamView *oamView = nullptr;
    CodeView *codeView = nullptr;
    CommandView *cmdView = nullptr;
    WatchView *watchView = nullptr;
    std::vector<std::unique_ptr<View>> closedViews; // deleted after the frame (a view may close its own window)
    PaneId focusedPane = kCommand, focusRequest = kCommand, newFocus = kCommand;
    PaneId dragPane = kNoPane; // the window the left button was pressed in (it gets the drag)

    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    bool imguiReady = false;
    std::string error;
    float cw = 8, ch = 16; // a character cell (pixels)
    float screenW = 0, screenH = 0;
    int fontSize = 16;
    int menuOpen = -1; // the menu shown (an index of menus()), -1: none
    int menuItem = 0;  // its selected item
    int menuX[8] = {}; // where each menu's title starts on the menu bar (columns)
    PaneId focusBeforeMenu = kCommand;
    bool ranLastFrame = false; // the game was running at the last frame's input (handleInput())
    bool sizing = false;       // Ctrl+F7: the arrow keys move the window, Shift+arrows size it
    bool quitRequested = false, done = false;
    bool dialogClosed = false;                   // (runDialog())
    int closeCount = 0;                          // close-button clicks on the debugger window (ends a dialog)
    std::vector<std::function<void()>> deferred; // run after the frame (they may run dialogs, which draw frames)
};
