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
#include "app.hpp"
#include <SDL.h>
#include <cctype>
#include <deque>
#include <cstdint>
#include <functional>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <imgui_internal.h> // (the title bar's rectangle, for the window number)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <utility>
#include <initializer_list>
#include <iterator>
#include "../core.hpp"
#include "../viewer_window.hpp"
#include "../remote.hpp"
#include "../settings.hpp"
#include "../emulator.hpp"
#include "../quickref.hpp"
#include "../watches.hpp"
#include "../window_places.hpp"
#include "debugger/theme.hpp"
#include "debugger/symbols.hpp"
#include "frontend.h" // (fe_rom_path(), fe_recent(), fe_sdl_window())

namespace {
constexpr int kTopHeight = 4;     // registers / PPU windows (2 rows + frame)
constexpr int kMemoryHeight = 10; // CPU and PPU memory windows, side by side (8 rows + frame)

// "~F~ile" -> "File"
std::string plain(const std::string &text)
{
    std::string out;
    for (char c : text) {
        if (c != '~') {
            out += c;
        }
    }
    return out;
}

// The key of "~F~ile" ('f'); 0 if none
char hotkey(const std::string &text)
{
    size_t at = text.find('~');
    return (at != std::string::npos && at + 1 < text.size())
               ? static_cast<char>(std::tolower(static_cast<unsigned char>(text[at + 1])))
               : 0;
}

// TEXT at (x, y), the part between '~' in HIGHLIGHT (Turbo Vision's moveCStr); returns the columns it took
int putTilde(Canvas &canvas, int x, int y, const std::string &text, const ThemeAttr &normal, const ThemeAttr &highlight)
{
    bool on = false;
    int start = x;
    std::string piece;
    auto flush = [&] {
        x += canvas.text(x, y, piece, on ? highlight : normal);
        piece.clear();
    };
    for (char c : text) {
        if (c == '~') {
            flush();
            on = !on;
        } else {
            piece += c;
        }
    }
    flush();
    return x - start;
}

std::string directoryOf(const std::string &file)
{
    return std::filesystem::path(file).parent_path().string();
}

// The status line: the visible keys (as on the quick reference). It only shows them: the mouse does nothing there.
const char *const kStatusItems[] = {
    "~F1~ Help", "~F5~ Go",    "~F6~ EC",   "~F7~ Here",      "~F8~ Trace",
    "~F9~ Bpx",  "~F10~ Step", "~F12~ Ret", "~Ctrl+D~ Break", "~Alt+X~ Exit",
};

} // namespace

// --- start, frames, events ---------------------------------------------------------------------------------------

DebuggerApp::DebuggerApp(Symbols &symbols, std::deque<std::string> script,
                         const std::vector<std::string> &startupMessages)
    : symbols(symbols), disasm(symbols), commands(*this, symbols)
{
    panes[kRegisters].title = "Registers";
    panes[kPpu].title = "PPU";
    panes[kCpuMemory].title = "CPU Memory";
    panes[kPpuMemory].title = "PPU Memory";
    panes[kCode].title = "Code";
    panes[kCommand].title = "Command";
    panes[kOam].title = "OAM";
    panes[kWatch].title = "Watch";
    if (!openWindow()) {
        return;
    }
    commands.enqueue(std::vector<std::string>(script.begin(), script.end())); // (run by idle(), one at a time)
    const bool game = Emulator::instance().hasRom();
    if (game) {
        createWindows();
    } else {
        openCommand(); // (the other windows show the game: they open once there is one)
    }
    // Messages from the emulator can arrive while it runs: queue them, idle() shows them. Only warnings and errors:
    // the command window is for the debugger's own output; info (a gamepad connected, a state saved...), debug and
    // trace messages only go to the log file. (Game-window keys answer on the game window: video_overlay().)
    log_set_hook(
        [](log_category category, log_level level, const char *message, void *context) {
            if (level <= LOG_LEVEL_WARN || (category == LOG_CAT_SCRIPT && level <= LOG_LEVEL_INFO)) {
                static_cast<DebuggerApp *>(context)->notices.emplace_back(message);
            }
        },
        this);
    // DebuggerApp:: -- this class's own versions, called from the constructor on purpose
    for (const std::string &message : startupMessages) {
        DebuggerApp::print(message);
    }
    if (game) {
        commands.loadWatches(); // (opens the watch window if there are any)
    } else {
        // started without a game (from a file manager, say): choose one
        DebuggerApp::print("No game loaded: File > Open game (F3), a recent game in the File menu, or load file.nes");
        defer([this] { doCommand(cmOpenRom); });
    }
    DebuggerApp::stopped();
}

DebuggerApp::~DebuggerApp()
{
    shutDown();
}

// The debugger's window: beside the game window when the screen has room for both, else in the middle
bool DebuggerApp::openWindow()
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        error = std::string("cannot start SDL video: ") + SDL_GetError();
        return false;
    }
    SDL_Rect usable{0, 0, 1280, 860};
    SDL_GetDisplayUsableBounds(0, &usable);
    fontSize = (usable.h >= 1400) ? 20 : 16;
    int w = std::min(1280 * fontSize / 16, usable.w - 40), h = std::min(860 * fontSize / 16, usable.h - 60);
    int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED;
    auto *game = static_cast<SDL_Window *>(fe_sdl_window());
    int gameW = 0, gameH = 0;
    // where the windows were when the debugger last quit (main.cpp has put the game window back already); else
    // the two side by side
    const WindowPlace mine = savedWindowPlace("debugger");
    if (mine.known) {
        x = mine.x;
        y = mine.y;
        w = mine.w;
        h = mine.h;
    } else if (game != nullptr && !savedWindowPlace("game").known) {
        SDL_GetWindowSize(game, &gameW, &gameH);
        if (usable.w >= w + gameW + 24) {
            x = usable.x + (usable.w - w - gameW - 16) / 2;
            y = usable.y + (usable.h - h) / 2;
            SDL_SetWindowPosition(game, x + w + 16, usable.y + (usable.h - gameH) / 2);
        }
    }
    window = SDL_CreateWindow("ONE Debugger", x, y, w, h, SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        error = std::string("cannot open the debugger's window: ") + SDL_GetError();
        return false;
    }
    if (mine.maximized) {
        SDL_MaximizeWindow(window);
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED); // (no vsync: the game sets the pace)
    if (renderer == nullptr) {
        renderer = SDL_CreateRenderer(window, -1, 0);
    }
    if (renderer == nullptr) {
        error = std::string("cannot draw in the debugger's window: ") + SDL_GetError();
        return false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imguiReady = true;
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini: the layout comes from here, as with Turbo Vision
    io.LogFilename = nullptr;
    io.ConfigWindowsMoveFromTitleBarOnly = true; // (a click in a window is for the window: the code cursor...)
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyNext = 0; // (Ctrl+Tab: not ImGui's window switcher)
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyPrev = 0;
    ImFontConfig font;
    font.PixelSnapH = true; // (whole pixels per character: a crisp grid)
    io.Fonts->AddFontDefaultVector(&font);
    setStyle();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);
    input_set_event_hook(&DebuggerApp::eventHook, this);

    // a first frame, for the font's size and the screen's (the windows are laid out in characters)
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    updateMetrics();
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    return true;
}

void DebuggerApp::shutDown()
{
    if (window != nullptr && error.empty()) {
        saveWindowPlace("debugger", window); // (it opens there next time)
    }
    log_set_hook(nullptr, nullptr);
    input_set_event_hook(nullptr, nullptr);
    if (imguiReady) {
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        imguiReady = false;
    }
    if (renderer != nullptr) {
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }
    if (window != nullptr) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
}

// The window frames, menus and status line in the theme's colors (frame_active, menu...); the windows' insides
// are drawn by the views
void DebuggerApp::setStyle()
{
    ImGuiStyle &style = ImGui::GetStyle();
    style = ImGuiStyle();
    style.FontSizeBase = static_cast<float>(fontSize);
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.FrameRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.GrabRounding = 0.0f;
    auto color = [](uint32_t rgb, float alpha = 1.0f) {
        return ImVec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                      static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
    };
    auto fg = [](const ThemeAttr &a) { return a.fgDefault ? 0xAAAAAAu : a.fg; };
    auto bg = [](const ThemeAttr &a) { return a.bgDefault ? 0x000000u : a.bg; };
    const ThemeAttr &text = themeColor(ThemeColor::Text);
    const ThemeAttr &active = themeColor(ThemeColor::FrameActive);
    const ThemeAttr &inactive = themeColor(ThemeColor::FrameInactive);
    const ThemeAttr &icons = themeColor(ThemeColor::FrameIcons);
    ImVec4 *c = style.Colors;
    c[ImGuiCol_WindowBg] = color(bg(text));
    c[ImGuiCol_TitleBg] = color(bg(inactive));
    c[ImGuiCol_TitleBgActive] = color(bg(active));
    c[ImGuiCol_TitleBgCollapsed] = color(bg(inactive));
    c[ImGuiCol_Border] = color(fg(inactive));
    c[ImGuiCol_Text] = color(fg(text));
    c[ImGuiCol_Button] = color(0, 0.0f);
    c[ImGuiCol_ButtonHovered] = color(fg(icons), 0.35f);
    c[ImGuiCol_ButtonActive] = color(fg(icons), 0.6f);
    c[ImGuiCol_ResizeGrip] = color(fg(inactive), 0.25f);
    c[ImGuiCol_ResizeGripHovered] = color(fg(active), 0.6f);
    c[ImGuiCol_ResizeGripActive] = color(fg(active), 0.9f);
}

// The size of a character cell and of the screen, for this frame
void DebuggerApp::updateMetrics()
{
    ch = std::round(ImGui::GetFontSize());
    cw = std::max(1.0f, std::round(ImGui::CalcTextSize("M").x));
    const ImGuiIO &io = ImGui::GetIO();
    screenW = io.DisplaySize.x;
    screenH = io.DisplaySize.y;
    ImGuiStyle &style = ImGui::GetStyle();
    // a window's frame takes a character all round, as in Turbo Vision: the title bar one row, the sides and the
    // bottom half a row and a column each
    style.FramePadding = ImVec2(std::round(cw * 0.5f), 0.0f);
    style.WindowPadding = ImVec2(cw, std::round(ch * 0.5f));
    style.WindowMinSize = ImVec2(16 * cw, 3 * ch);
}

bool DebuggerApp::eventHook(void *sdlEvent, void *context)
{
    return static_cast<DebuggerApp *>(context)->sdlEvent(*static_cast<SDL_Event *>(sdlEvent));
}

// Every SDL event comes here first (input_poll()): the debugger window's go to ImGui (true: the game window does
// not see them). The game window's close button quits through input.c (GameKey::kQuit), as with Turbo Vision.
bool DebuggerApp::sdlEvent(SDL_Event &event)
{
    if (ViewerWindow::handleEventAll(event)) {
        return true; // (a picture window's: wbg, wpal)
    }
    if (isHandbackButton(&event)) { // (handback_button: stops the game like Ctrl+D)
        Emulator::instance().stop();
        return true;
    }
    Uint32 id = 0;
    switch (event.type) {
    case SDL_WINDOWEVENT: id = event.window.windowID; break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: id = event.key.windowID; break;
    case SDL_TEXTEDITING: id = event.edit.windowID; break;
    case SDL_TEXTINPUT: id = event.text.windowID; break;
    case SDL_MOUSEMOTION: id = event.motion.windowID; break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: id = event.button.windowID; break;
    case SDL_MOUSEWHEEL: id = event.wheel.windowID; break;
    case SDL_QUIT: // (the last window closed, or Ctrl+C in the terminal)
        quitRequested = true;
        closeCount++;
        return true;
    default:
        if (imguiReady) {
            ImGui_ImplSDL2_ProcessEvent(&event); // (gamepads, the clipboard...: the game window gets them too)
        }
        return false;
    }
    if (window != nullptr && id == SDL_GetWindowID(window)) {
        if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) {
            quitRequested = true;
            closeCount++;
        }
        if (imguiReady) {
            ImGui_ImplSDL2_ProcessEvent(&event);
        }
        return true;
    }
    return false; // (another window's: the game window's close button quits through input.c, as in one)
}

void DebuggerApp::frame(Dialog *dialog)
{
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0) {
        return;
    }
    ImGuiIO &io = ImGui::GetIO();
    // ImGui's keyboard navigation only in the dialogs: elsewhere the keys are the debugger's
    if (dialog != nullptr) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    }
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    updateMetrics();
    if (dialog == nullptr) {
        handleInput();
    }
    drawDesktop();
    newFocus = kNoPane;
    for (int id = kRegisters; id < kPanes; id++) {
        drawPane(static_cast<PaneId>(id));
    }
    if (menuOpen < 0 && dialog == nullptr) {
        focusedPane = newFocus;
    }
    drawMenuBar();
    drawStatusLine();
    if (dialog != nullptr && !dialog->draw()) {
        dialogClosed = true;
    }
    ImGui::Render();
    const ThemeAttr &desk = themeColor(ThemeColor::Desktop);
    uint32_t rgb = desk.bgDefault ? 0 : desk.bg;
    SDL_SetRenderDrawColor(renderer, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 0xFF);
    SDL_RenderClear(renderer);
    SDL_RenderSetScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    capture();
    SDL_RenderPresent(renderer);
    closedViews.clear();
}

// ONE_DEBUGGER_CAPTURE=file.bmp: the window, saved while the debugger waits (stopped, no commands queued) -- for
// tests and the quick reference's screen captures (also with SDL_VIDEODRIVER=dummy)
void DebuggerApp::capture()
{
    const char *file = std::getenv("ONE_DEBUGGER_CAPTURE");
    if (file == nullptr || *file == '\0' || Emulator::instance().running() ||
        (commands.hasPending() && !commands.nextNeedsRom())) {
        return;
    }
    int w = 0, h = 0;
    if (SDL_GetRendererOutputSize(renderer, &w, &h) != 0) {
        return;
    }
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        return;
    }
    if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch) == 0) {
        std::string temporary = std::string(file) + ".part"; // (renamed when complete: never half a picture)
        if (SDL_SaveBMP(surface, temporary.c_str()) == 0) {
            std::rename(temporary.c_str(), file);
        }
    }
    SDL_FreeSurface(surface);
}

// A dialog box: frames until it closes (the emulation waits, as do the queued commands)
void DebuggerApp::runDialog(Dialog &dialog)
{
    if (window == nullptr) {
        return;
    }
    if (menuOpen >= 0) {
        closeMenu();
    }
    const int closes = closeCount;
    dialogClosed = false;
    while (!dialogClosed && closeCount == closes) { // (the window's close button cancels it)
        SDL_WaitEventTimeout(nullptr, 50);
        input_poll();
        frame(&dialog);
    }
}

// --- the main loop -----------------------------------------------------------------------------------------------

void DebuggerApp::run()
{
    if (window == nullptr) {
        return;
    }
    uint32_t lastFrame = 0;
    bool busy = true; // something happened: draw now, without waiting for an event
    while (!done) {
        Emulator &emu = Emulator::instance();
        if (!emu.running() && !busy && (!commands.hasPending() || commands.nextNeedsRom())) {
            SDL_WaitEventTimeout(nullptr, 50); // nothing to do: wait for an event (or the cursor's next blink)
        }
        busy = false;
        idle();
        if (quitRequested) {
            quitRequested = false;
            if (confirmQuit()) {
                done = true;
                break;
            }
        }
        if (emu.running()) { // (the game window shows every frame; this one is drawn 30 times a second)
            uint32_t now = SDL_GetTicks();
            if (now - lastFrame < 33) {
                continue;
            }
            lastFrame = now;
        }
        frame(nullptr);
        if (!deferred.empty()) {
            std::vector<std::function<void()>> actions;
            actions.swap(deferred);
            for (auto &action : actions) {
                action();
            }
            busy = true;
        }
    }
}

void DebuggerApp::idle()
{
    while (!notices.empty()) { // (already in the log file: log_write() put them there)
        std::string notice = notices.front();
        notices.pop_front();
        if (cmdView == nullptr) {
            openCommand();
        }
        cmdView->print(notice);
    }
    RemoteServer::instance().poll(commands); // (the MCP adapter's requests, with --remote)
    Emulator &emu = Emulator::instance();
    if (emu.running()) {
        StopReason reason = emu.runSlice();
        ViewerWindow::refreshAll(); // (the picture windows are live: a frame each slice)
        if (reason != StopReason::None) {
            if (cmdView) {
                cmdView->setRunning(false);
            }
            std::optional<GameKey> key = emu.takeGameKey();
            int step = afterStop;
            afterStop = 0;
            if (step != 0 && !key && reason == StopReason::UserBreak) {
                stopped();       // (stopped for the key: the step reports where it ends)
                doCommand(step); // F8 / F10 / F12 pressed in the debugger while the game ran
                return;
            }
            if (!key || reason != StopReason::UserBreak) {
                commands.reportStop(reason); // (a stop for a game window key is announced by gameKey())
            } else {
                stopped();
            }
            if (key) {
                gameKey(*key);
            }
        }
        return;
    }
    input_poll();                                 // the game window and this one (SDL events, debugger keys)
    if (savestate_load_count() != loadedStates) { // F2 in the game window loaded a state: show it
        loadedStates = savestate_load_count();
        stopped();
    }
    if (std::optional<GameKey> key = emu.takeGameKey()) {
        gameKey(*key);
        return;
    }
    if (commands.hasPending() && !commands.nextNeedsRom()) { // (the -s commands wait for a game)
        commands.execute(commands.takePending());
    }
}

int DebuggerApp::exitCode() const
{
    if (!error.empty()) {
        return 2;
    }
    return commands.assertionFailed() ? 1 : 0;
}

// --- the desktop and its windows ---------------------------------------------------------------------------------

DebuggerApp::Cells DebuggerApp::desktop() const
{
    Cells d;
    d.bx = std::max(20, static_cast<int>(screenW / cw));
    d.by = std::max(10, static_cast<int>(screenH / ch) - 2); // (the menu bar and the status line)
    return d;
}

DebuggerApp::Cells DebuggerApp::cellsOf(const Pane &p) const
{
    Cells r;
    r.ax = static_cast<int>(std::lround(p.x / cw));
    r.ay = static_cast<int>(std::lround((p.y - ch) / ch));
    r.bx = r.ax + static_cast<int>(std::lround(p.w / cw));
    r.by = r.ay + static_cast<int>(std::lround(p.h / ch));
    return r;
}

void DebuggerApp::place(PaneId id, const Cells &where)
{
    Pane &p = pane(id);
    p.place = where;
    p.placePending = true;
    p.x = where.ax * cw; // (known now: the layout compares them before the next frame)
    p.y = ch + where.ay * ch;
    p.w = (where.bx - where.ax) * cw;
    p.h = (where.by - where.ay) * ch;
    if (p.view) {
        p.view->cols = std::max(1, where.bx - where.ax - 2);
        p.view->rows = std::max(1, where.by - where.ay - 2);
    }
}

void DebuggerApp::openPane(PaneId id, const Cells &where, std::unique_ptr<View> view)
{
    Pane &p = pane(id);
    p.open = true;
    p.zoomed = false;
    p.view = std::move(view);
    place(id, where);
    select(id);
}

// A window closed (its close button, a command, the layout): forget it and its view
void DebuggerApp::closePane(PaneId id)
{
    Pane &p = pane(id);
    if (!p.open) {
        return;
    }
    p.open = false;
    closedViews.push_back(std::move(p.view)); // (it may be the one asking: deleted after the frame)
    switch (id) {
    case kRegisters: regView = nullptr; break;
    case kPpu: ppuView = nullptr; break;
    case kCpuMemory: memView = nullptr; break;
    case kPpuMemory: ppuMemView = nullptr; break;
    case kCode: codeView = nullptr; break;
    case kCommand: cmdView = nullptr; break;
    case kOam: oamView = nullptr; break;
    case kWatch: {
        watchView = nullptr;
        Cells narrow = codeArea();
        narrow.bx = watchArea().ax;
        if (pane(kCode).open && cellsOf(pane(kCode)) == narrow) {
            place(kCode, codeArea()); // the code window gets its width back
        }
        break;
    }
    default: break;
    }
    if (focusRequest == id) {
        focusRequest = kNoPane;
    }
    if (dragPane == id) {
        dragPane = kNoPane;
    }
}

DebuggerApp::PaneId DebuggerApp::focused() const
{
    return focusRequest != kNoPane ? focusRequest : focusedPane;
}

View *DebuggerApp::focusedView()
{
    PaneId id = focused();
    return (id > 0 && id < kPanes && pane(id).open) ? pane(id).view.get() : nullptr;
}

void DebuggerApp::drawDesktop()
{
    // Turbo Vision's desktop is a pattern (light shade): its color is a quarter foreground, the rest background
    const ThemeAttr &desk = themeColor(ThemeColor::Desktop);
    uint32_t f = desk.fgDefault ? 0xAAAAAA : desk.fg, b = desk.bgDefault ? 0 : desk.bg, mix = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        mix |= ((((f >> shift) & 0xFF) + 3 * ((b >> shift) & 0xFF)) / 4) << shift;
    }
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, ch), ImVec2(screenW, screenH - ch), rgbColor(mix));
}

void DebuggerApp::drawPane(PaneId id)
{
    Pane &p = pane(id);
    if (!p.open) {
        return;
    }
    if (p.placePending) {
        ImGui::SetNextWindowPos(ImVec2(p.x, p.y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(p.w, p.h), ImGuiCond_Always);
        p.placePending = false;
    }
    if (focusRequest == id && menuOpen < 0) {
        ImGui::SetNextWindowFocus();
        focusRequest = kNoPane;
        focusedPane = id;
    }
    const bool isFocused = focusedPane == id && menuOpen < 0;
    const ThemeAttr &frameColor = themeColor(isFocused ? ThemeColor::FrameActive : ThemeColor::FrameInactive);
    ImGui::PushStyleColor(ImGuiCol_Text, foreground(frameColor));
    ImGui::PushStyleColor(ImGuiCol_Border, foreground(frameColor));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (menuOpen >= 0) {
        flags |= ImGuiWindowFlags_NoInputs; // (the mouse is the menu's)
    }
    char name[96];
    std::string note = (id == kCpuMemory && memView != nullptr) ? memView->bankNote() : std::string();
    std::snprintf(name, sizeof(name), "###pane%d", static_cast<int>(id)); // (the title is drawn below, a little higher)
    const std::string title = std::string(p.title) + note;
    bool keep = true;
    bool visible = ImGui::Begin(name, &keep, flags);
    ImGui::PopStyleColor(); // (the title was drawn with it)
    ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    if (pos.y < ch) { // (not over the menu bar)
        pos.y = ch;
        ImGui::SetWindowPos(pos);
    }
    p.x = pos.x;
    p.y = pos.y;
    p.w = size.x;
    p.h = size.y;
    if (ImGui::IsWindowFocused()) {
        newFocus = id;
    }
    if (visible) {
        ImGuiWindow *w = ImGui::GetCurrentWindow();
        // the window's number at the right of the title bar, as Turbo Vision shows it
        ImRect bar = w->TitleBarRect();
        char number[8];
        std::snprintf(number, sizeof(number), "%d", static_cast<int>(id));
        ImGui::PushClipRect(bar.Min, bar.Max, false);
        w->DrawList->AddText(ImVec2(std::round(bar.Max.x - ch - 3 * cw), bar.Min.y - std::round(ch * 0.1f)),
                             foreground(frameColor), number);
        // the title, centered, higher than ImGui would put it so that g, p, q, y keep their tails
        const float titleW = ImGui::CalcTextSize(title.c_str()).x;
        w->DrawList->AddText(
            ImVec2(std::round(bar.Min.x + (bar.GetWidth() - titleW) * 0.5f), bar.Min.y - std::round(ch * 0.1f)),
            foreground(frameColor), title.c_str());
        ImGui::PopClipRect();

        ImVec2 origin = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
        origin = ImVec2(std::round(origin.x), std::round(origin.y));
        int cols = std::max(1, static_cast<int>(avail.x / cw + 0.01f));
        int rows = std::max(1, static_cast<int>(avail.y / ch + 0.01f));
        View *view = p.view.get();
        view->cols = cols;
        view->rows = rows;
        // While the game runs the window shows what it showed at the last stop (SoftICE): its recording, and the
        // mouse leaves it alone. Stopped, it is drawn from the emulator's state, and recorded.
        const bool frozen = Emulator::instance().running() && !view->drawnWhileRunning();
        // the mouse (not while the game runs): a click (a double click), dragging after it, the wheel; the middle
        // button in the command window pastes the X11 primary selection
        const ImGuiIO &io = ImGui::GetIO();
        const bool mouseAllowed = menuOpen < 0 && !Emulator::instance().running();
        const int x = static_cast<int>(std::floor((io.MousePos.x - origin.x) / cw));
        const int y = static_cast<int>(std::floor((io.MousePos.y - origin.y) / ch));
        if (mouseAllowed && ImGui::IsWindowHovered()) {
            if (io.MouseWheel != 0.0f) {
                view->wheel(io.MouseWheel > 0 ? -3 : 3);
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && x >= 0 && y >= 0 && x < cols && y < rows) {
                view->click(x, y, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
                dragPane = id;
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && view == cmdView) {
                pastePrimary();
            }
        }
        if (dragPane == id) {
            if (mouseAllowed && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                view->drag(x, y);
            } else {
                view->release();
                dragPane = kNoPane;
            }
        }
        Canvas canvas(ImGui::GetWindowDrawList(), origin, ImVec2(cw, ch), cols, rows);
        if (frozen) {
            for (int y = 0; y < rows; y++) {
                canvas.fill(0, y, cols, themeColor(ThemeColor::Text)); // (opened during the run: empty until it stops)
            }
            canvas.replay(view->stopped);
        } else {
            view->stopped.clear();
            canvas.record = &view->stopped;
            view->draw(canvas, isFocused);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    if (!keep) {
        closePane(id);
    }
}

void DebuggerApp::openRegisters()
{
    if (pane(kRegisters).open) {
        select(kRegisters);
        return;
    }
    auto view = std::make_unique<RegistersView>();
    regView = view.get();
    openPane(kRegisters, Cells{0, 0, leftColumnWidth(), kTopHeight}, std::move(view));
}

void DebuggerApp::openPpu()
{
    if (pane(kPpu).open) {
        select(kPpu);
        return;
    }
    Cells d = desktop();
    int left = leftColumnWidth();
    auto view = std::make_unique<PpuView>();
    ppuView = view.get();
    openPane(kPpu, Cells{left, 0, std::max(d.bx, left + 40), kTopHeight}, std::move(view));
}

void DebuggerApp::openCpuMemory()
{
    if (pane(kCpuMemory).open) {
        select(kCpuMemory);
        return;
    }
    auto view = std::make_unique<MemoryView>(MemoryView::Source::Cpu);
    memView = view.get();
    openPane(kCpuMemory, Cells{0, kTopHeight, leftColumnWidth(), kTopHeight + kMemoryHeight}, std::move(view));
}

void DebuggerApp::openCommand()
{
    if (pane(kCommand).open) {
        select(kCommand);
        return;
    }
    Cells d = desktop();
    int height = std::max(8, d.by / 4);
    auto view = std::make_unique<CommandView>(
        [this](const std::string &line) { defer([this, line] { commands.execute(line); }); });
    cmdView = view.get();
    cmdView->onComplete = [this](const std::string &text) { return commands.complete(text); };
    cmdView->onInputChanged = [this](const std::string &text) { hint = commands.hint(text); };
#if SDL_VERSION_ATLEAST(2, 26, 0)
    cmdView->onSelected = [](const std::string &text) { SDL_SetPrimarySelectionText(text.c_str()); };
#endif
    openPane(kCommand, Cells{0, d.by - height, d.bx, d.by}, std::move(view));
}

// where the code window goes in the default layout: full width between the memory windows and the command window
DebuggerApp::Cells DebuggerApp::codeArea() const
{
    Cells d = desktop();
    int height = std::max(8, d.by / 4);
    return Cells{0, kTopHeight + kMemoryHeight, d.bx, std::max(kTopHeight + kMemoryHeight + 3, d.by - height)};
}

void DebuggerApp::openCode()
{
    if (pane(kCode).open) {
        select(kCode);
        return;
    }
    Cells r = codeArea();
    if (pane(kWatch).open && cellsOf(pane(kWatch)) == watchArea()) {
        r.bx = watchArea().ax; // beside the watch window
    }
    auto view = std::make_unique<CodeView>(disasm, symbols);
    codeView = view.get();
    codeView->onComment = [this](uint16_t address) {
        // ';' on the cursor line: the comment command with the comment there now, to edit and enter
        std::string text;
        if (const std::string *comment = symbols.commentAt(address)) {
            text = *comment;
            for (size_t i = 0; (i = text.find('\n', i)) != std::string::npos; i += 2) {
                text.replace(i, 1, "\\n");
            }
        }
        char head[24];
        std::snprintf(head, sizeof(head), "comment %04X ", address);
        if (cmdView == nullptr) {
            openCommand();
            select(kCode); // (the cursor line stays visible)
        }
        cmdView->setInput(head + text);
    };
    codeView->onToggleBreakpoint = [this] { toggleBreakpointAtCursor(); };
#if SDL_VERSION_ATLEAST(2, 26, 0)
    codeView->onSelected = [](const std::string &text) { SDL_SetPrimarySelectionText(text.c_str()); };
#endif
    openPane(kCode, r, std::move(view));
    codeView->onStop();
}

// where the watch window goes: the right part of the code area, as tall as the code window (which gets narrower)
DebuggerApp::Cells DebuggerApp::watchArea() const
{
    Cells r = codeArea();
    int width = std::min(50, std::max(30, r.bx - 50)); // the code window keeps at least 50 columns
    r.ax = std::max(0, r.bx - width);
    return r;
}

// Windows 1-4 in two columns of the same width: half the screen each. A narrow screen gives the left column what
// the registers need (54 columns) and the right column the rest, at least 40.
int DebuggerApp::leftColumnWidth() const
{
    int width = desktop().bx;
    return (width / 2 >= 54) ? width / 2 : std::max(40, std::min(54, width - 40));
}

// where the OAM window opens: the right part of the code area
DebuggerApp::Cells DebuggerApp::sideArea() const
{
    Cells d = desktop();
    int height = std::max(8, d.by / 4);
    int top = kTopHeight + kMemoryHeight;
    int bottom = std::max(top + 6, d.by - height);
    return Cells{std::max(0, d.bx - 56), top, d.bx, bottom};
}

void DebuggerApp::openPpuMemory()
{
    if (pane(kPpuMemory).open) {
        select(kPpuMemory);
        return;
    }
    Cells d = desktop();
    auto view = std::make_unique<MemoryView>(MemoryView::Source::Ppu);
    ppuMemView = view.get();
    // right of the CPU memory window
    openPane(kPpuMemory, Cells{leftColumnWidth(), kTopHeight, d.bx, kTopHeight + kMemoryHeight}, std::move(view));
}

void DebuggerApp::openOam()
{
    if (pane(kOam).open) {
        select(kOam);
        return;
    }
    auto view = std::make_unique<OamView>();
    oamView = view.get();
    oamView->onEdit = [this](const std::string &text) {
        openCommand(); // (the values are typed there)
        cmdView->setInput(text);
    };
    openPane(kOam, sideArea(), std::move(view)); // (decoded: 3 header rows, then the sprites)
}

// The watch window opens to the right of the code window, which is made narrower to make room (SoftICE's windows
// never overlap). Only a code window still where the default layout put it is resized: one the user has moved or
// resized is left alone. Closing the watch window gives the code window its width back.
void DebuggerApp::openWatches()
{
    if (pane(kWatch).open) {
        return;
    }
    Cells r = watchArea();
    auto view = std::make_unique<WatchView>(symbols);
    watchView = view.get();
    watchView->onEdit = [this](const std::string &text) {
        openCommand(); // (selects it: the value is typed there, and Enter must reach the command line)
        cmdView->setInput(text);
    };
    watchView->onCommand = [this](const std::string &command) {
        defer([this, command] { commands.execute(command); });
    };
    openPane(kWatch, r, std::move(view));
    Cells narrow = codeArea();
    narrow.bx = r.ax;
    if (pane(kCode).open && cellsOf(pane(kCode)) == codeArea()) {
        place(kCode, narrow);
    }
}

void DebuggerApp::toggleWatchWindow()
{
    if (pane(kWatch).open) {
        closePane(kWatch);
    } else {
        openWatches();
        if (cmdView)
            select(kCommand);
    }
}

void DebuggerApp::watchesChanged(bool added)
{
    if (Watches::instance().empty()) {
        closePane(kWatch); // the last watch went: close the window
        return;
    }
    if (added && !pane(kWatch).open) {
        openWatches();
        if (cmdView)
            select(kCommand); // the command line keeps the focus
        return;
    }
    if (watchView) {
        watchView->listChanged();
    }
}

void DebuggerApp::createWindows()
{
    openRegisters();
    openPpu();
    openCpuMemory();
    openPpuMemory();
    openCode();
    openCommand(); // last: it has the focus
}

// load: a new game is on (or none, if it could not be loaded): its windows (the File menu is made as it opens)
void DebuggerApp::romLoaded()
{
    if (!Emulator::instance().hasRom()) {
        closeGameWindows();
    } else if (!pane(kRegisters).open && !pane(kPpu).open && !pane(kCpuMemory).open && !pane(kCode).open) {
        createWindows(); // (the first game)
    } else if (codeView) {
        codeView->reset(); // (anchors in the other game's code)
    }
}

void DebuggerApp::closeGameWindows()
{
    for (PaneId id : {kRegisters, kPpu, kCpuMemory, kCode, kPpuMemory, kOam, kWatch}) {
        closePane(id);
    }
}

void DebuggerApp::defaultLayout()
{
    if (!Emulator::instance().hasRom()) {
        closeGameWindows();
        if (pane(kCommand).open) {
            place(kCommand, Cells{0, desktop().by - std::max(8, desktop().by / 4), desktop().bx, desktop().by});
        }
        openCommand();
        return;
    }
    // (the command window keeps what it shows: only its place changes)
    for (PaneId id : {kRegisters, kPpu, kCpuMemory, kCode, kPpuMemory, kOam, kWatch}) {
        closePane(id);
    }
    if (pane(kCommand).open) {
        Cells d = desktop();
        int height = std::max(8, d.by / 4);
        place(kCommand, Cells{0, d.by - height, d.bx, d.by});
        pane(kCommand).zoomed = false;
    }
    createWindows();
    if (!Watches::instance().empty()) {
        openWatches();
        if (cmdView)
            select(kCommand);
    }
    stopped();
}

// Ctrl+F6: the next window (by number) gets the keyboard
void DebuggerApp::nextWindow()
{
    int start = focused();
    for (int i = 1; i < kPanes; i++) {
        int id = (start + i - 1) % (kPanes - 1) + 1;
        if (pane(static_cast<PaneId>(id)).open) {
            select(static_cast<PaneId>(id));
            return;
        }
    }
}

// Ctrl+F5: the window with the keyboard takes the whole desktop, or goes back to where it was
void DebuggerApp::zoomWindow()
{
    PaneId id = focused();
    if (id == kNoPane || !pane(id).open) {
        return;
    }
    Pane &p = pane(id);
    if (p.zoomed) {
        place(id, p.beforeZoom);
        p.zoomed = false;
    } else {
        p.beforeZoom = cellsOf(p);
        place(id, desktop());
        p.zoomed = true;
    }
}

// Window > Tile: the open windows side by side, in rows, filling the desktop
void DebuggerApp::tileWindows()
{
    std::vector<PaneId> open;
    for (int id = kRegisters; id < kPanes; id++) {
        if (pane(static_cast<PaneId>(id)).open) {
            open.push_back(static_cast<PaneId>(id));
        }
    }
    if (open.empty()) {
        return;
    }
    const Cells d = desktop();
    const int n = static_cast<int>(open.size());
    const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    const int rowCount = (n + columns - 1) / columns;
    for (int i = 0; i < n; i++) {
        int row = i / columns, inRow = std::min(columns, n - row * columns), col = i % columns;
        Cells r{d.bx * col / inRow, d.by * row / rowCount, d.bx * (col + 1) / inRow, d.by * (row + 1) / rowCount};
        place(open[static_cast<size_t>(i)], r);
        pane(open[static_cast<size_t>(i)]).zoomed = false;
    }
}

// Window > Cascade: the open windows one over the other, each a row and a column further in
void DebuggerApp::cascadeWindows()
{
    const Cells d = desktop();
    int i = 0, n = 0;
    for (int id = kRegisters; id < kPanes; id++) {
        n += pane(static_cast<PaneId>(id)).open ? 1 : 0;
    }
    for (int id = kRegisters; id < kPanes; id++) {
        if (pane(static_cast<PaneId>(id)).open) {
            place(static_cast<PaneId>(id), Cells{i, i, d.bx - (n - 1 - i), d.by - (n - 1 - i)});
            pane(static_cast<PaneId>(id)).zoomed = false;
            i++;
        }
    }
    if (focused() != kNoPane) {
        select(focused()); // (on top)
    }
}

// Ctrl+= / Ctrl+-: larger or smaller characters. The windows stay where they are (fewer or more characters fit in
// them); Window > Default layout lays them out again for the new size.
void DebuggerApp::changeFontSize(int delta)
{
    fontSize = std::clamp(fontSize + delta, 10, 40);
    ImGui::GetStyle().FontSizeBase = static_cast<float>(fontSize);
}

// --- CommandHost -------------------------------------------------------------------------------------------------

void DebuggerApp::print(const std::string &line)
{
    if (cmdView == nullptr) {
        openCommand();
    }
    cmdView->print(line);
    std::printf("%s\n", line.c_str()); // the log file keeps a copy
}

void DebuggerApp::quit()
{
    quitRequested = true; // (run(): asks first if labels are unsaved)
}

void DebuggerApp::stateChanged()
{
    ViewerWindow::refreshAll(); // (wbg, wpal; the debugger's own windows are drawn every frame anyway)
}

void DebuggerApp::stopped()
{
    std::fflush(stdout); // bring the log file (and the -t trace in it) up to date at every stop
    std::fflush(stderr);
    if (frameInProgress) {
        DebuggerApp::showGameFrame(); // (and the title; DebuggerApp:: -- also called from the constructor)
    } else {
        video_set_title("ONE Debugger", "Stopped"); // (the window shows the last complete frame already)
    }
    for (View *v : std::initializer_list<View *>{regView, ppuView, memView, codeView, ppuMemView, oamView, watchView}) {
        if (v) {
            v->onStop();
        }
    }
    DebuggerApp::stateChanged();
}

void DebuggerApp::showMemory(uint16_t address, uint32_t mark)
{
    openCpuMemory();
    memView->showAddress(address, mark);
    if (cmdView)
        select(kCommand); // the command line keeps the focus
}

void DebuggerApp::showPpuMemory(uint16_t address, uint32_t mark)
{
    openPpuMemory();
    ppuMemView->showAddress(address, mark);
    if (cmdView)
        select(kCommand); // the command line keeps the focus
}

void DebuggerApp::showCode(uint16_t address)
{
    openCode();
    codeView->showAddress(address);
}

// SoftICE's EC (F6): move the keyboard between the command line and the code window. In the code window the
// arrow keys and PgUp/PgDn move the cursor bar through the code (F9 sets a breakpoint on it); typed commands
// still go to the command line.
void DebuggerApp::toggleCodeFocus()
{
    if (pane(kCode).open && focused() == kCode) {
        openCommand(); // (selects it)
    } else {
        openCode();
    }
}

std::optional<uint16_t> DebuggerApp::codeCursor()
{
    if (codeView && focused() == kCode) {
        return codeView->cursorAddress();
    }
    return std::nullopt;
}

void DebuggerApp::toggleOamWindow()
{
    if (pane(kOam).open) {
        closePane(kOam);
    } else {
        openOam();
        if (cmdView)
            select(kCommand);
    }
}

// rs (F4): the frame in progress at a stop, or the last complete frame; the title says which
void DebuggerApp::showGameFrame()
{
    if (Emulator::instance().running()) {
        return; // (the frames being drawn show themselves; the next stop shows the frame in progress)
    }
    if (frameInProgress) {
        uint32_t scanline = ppu_debug_get_scanline();
        video_show_in_progress(static_cast<int>(scanline), static_cast<int>(ppu_debug_get_scanline_clock()));
        char status[64];
        std::snprintf(status, sizeof(status), "Stopped - frame so far, line %u", scanline);
        video_set_title("ONE Debugger", status);
    } else {
        video_show_last_frame();
        video_set_title("ONE Debugger", "Stopped");
    }
}

void DebuggerApp::runStarted()
{
    if (cmdView) {
        cmdView->setRunning(true);
    }
    video_set_title("ONE Debugger", "Running");
}

// --- menus, status line, keys ------------------------------------------------------------------------------------

std::vector<DebuggerApp::MenuDef> DebuggerApp::menus() const
{
    std::vector<MenuDef> list;
    // File: open a game, the recent ones (the list one keeps too), exit
    MenuDef file{"~F~ile", {{"~O~pen game...", "F3", cmOpenRom}}};
    int recent = std::min(fe_recent_count(FE_RECENT_GAMES), cmRecentRomLast - cmRecentRom + 1);
    if (recent > 0) {
        file.items.push_back({});
    }
    for (int i = 0; i < recent; i++) {
        std::string name = std::filesystem::path(fe_recent(FE_RECENT_GAMES, i)).filename().string();
        file.items.push_back({"~" + std::to_string(i + 1) + "~ " + name, "", cmRecentRom + i});
    }
    file.items.push_back({});
    file.items.push_back({"E~x~it", "Alt+X", cmQuit});
    list.push_back(file);
    list.push_back({"~R~un",
                    {{"~G~o / stop", "F5", cmGo},
                     {"~T~race into", "F8", cmTrace},
                     {"Go to cursor (~h~ere)", "F7", cmHere},
                     {"~S~tep over", "F10", cmStepOver},
                     {"Step to ~r~eturn", "F12", cmStepReturn},
                     {"~B~reak", "Ctrl+D", cmBreak},
                     {},
                     {"Toggle break~p~oint", "F9", cmToggleBreakpoint},
                     {"~F~rame in progress", "F4", cmShowScreen},
                     {"Scree~n~shot", "Shift+F12", cmScreenshot},
                     {"~F~PS counter", "Shift+F10", cmFps},
                     {},
                     {"R~e~set", "", cmReset},
                     {"~P~ower cycle", "", cmPowerCycle},
                     {},
                     {"E~x~it", "Alt+X", cmQuit}}});
    list.push_back({"~W~indow",
                    {{"~R~egisters", "", cmWinRegisters},
                     {"~P~PU registers", "", cmWinPpu},
                     {"CPU ~M~emory", "", cmWinCpuMemory},
                     {"PPU M~e~mory", "", cmWinPpuMemory},
                     {"~C~ode", "", cmWinCode},
                     {"Comma~n~d", "", cmWinCommand},
                     {"~O~AM", "", cmWinOam},
                     {"~W~atch", "", cmWinWatch},
                     {},
                     {"~E~nter/exit code window", "F6", cmEnterCode},
                     {"~N~ext", "Ctrl+F6", cmNext},
                     {"~Z~oom", "Ctrl+F5", cmZoom},
                     {"~S~ize/move", "Ctrl+F7", cmResize},
                     {"~T~ile", "", cmTile},
                     {"C~a~scade", "", cmCascade},
                     {"~D~efault layout", "", cmDefaultLayout},
                     {},
                     {"~L~arger characters", "Ctrl+=", cmFontLarger},
                     {"Smaller c~h~aracters", "Ctrl+-", cmFontSmaller}}});
    list.push_back({"~L~ua",
                    {{"~R~un script...", "", cmLuaRun},
                     {"~S~top all scripts", "", cmLuaStop},
                     {"~L~ist running scripts", "", cmLuaList}}});
    list.push_back({"~H~elp", {{"~C~ommand help", "F1", cmHelpCommands}, {"~Q~uick Reference", "", cmQuickReference}}});
    return list;
}

// The menu bar (Turbo Vision's TMenuBar): " ~F~ile " from column 1, the open menu's title selected
void DebuggerApp::drawMenuBar()
{
    std::vector<MenuDef> list = menus();
    Canvas bar(ImGui::GetForegroundDrawList(), ImVec2(0, 0), ImVec2(cw, ch), static_cast<int>(screenW / cw) + 1, 1);
    bar.fill(0, 0, bar.cols, themeColor(ThemeColor::Menu));
    int x = 1;
    for (size_t i = 0; i < list.size() && i < std::size(menuX); i++) {
        const bool selected = static_cast<int>(i) == menuOpen;
        const ThemeAttr &normal = themeColor(selected ? ThemeColor::MenuSelected : ThemeColor::Menu);
        const ThemeAttr &key = themeColor(selected ? ThemeColor::MenuSelectedShortcut : ThemeColor::MenuShortcut);
        menuX[i] = x;
        int l = static_cast<int>(plain(list[i].title).size());
        bar.text(x, 0, " ", normal);
        putTilde(bar, x + 1, 0, list[i].title, normal, key);
        bar.text(x + l + 1, 0, " ", normal);
        x += l + 2;
    }
    if (menuOpen >= 0 && menuOpen < static_cast<int>(list.size())) {
        drawMenuBox(list[static_cast<size_t>(menuOpen)], menuX[menuOpen]);
    }
}

namespace {
// A menu's width (Turbo Vision's TMenuBox: the label + 6, + the key + 2)
int menuWidth(const std::vector<std::string> &labels, const std::vector<std::string> &keys)
{
    int w = 10;
    for (size_t i = 0; i < labels.size(); i++) {
        if (!labels[i].empty()) {
            int l = static_cast<int>(plain(labels[i]).size()) + 6;
            if (!keys[i].empty()) {
                l += static_cast<int>(keys[i].size()) + 2;
            }
            w = std::max(w, l);
        }
    }
    return w;
}
} // namespace

// A menu (Turbo Vision's TMenuBox) below its title: a single-line frame, the items from column 3, their keys at
// the right, the selected item highlighted between the frame's sides
void DebuggerApp::drawMenuBox(const MenuDef &menu, int x)
{
    std::vector<std::string> labels, keys;
    for (const MenuItemDef &item : menu.items) {
        labels.push_back(item.label);
        keys.push_back(item.key);
    }
    const int w = menuWidth(labels, keys);
    const int h = static_cast<int>(menu.items.size()) + 2;
    const int maxX = static_cast<int>(screenW / cw);
    x = std::max(0, std::min(x, maxX - w));
    Canvas box(ImGui::GetForegroundDrawList(), ImVec2(x * cw, ch), ImVec2(cw, ch), w, h);
    drawShadow(box.drawList, box.at(0, 0), box.at(w, h), box.cell);
    const ThemeAttr &normal = themeColor(ThemeColor::Menu);
    for (int y = 0; y < h; y++) {
        box.fill(0, y, w, normal);
    }
    const ImU32 line = foreground(normal);
    auto px = [&](float col) { return std::round(box.origin.x + col * cw); };
    auto py = [&](float row) { return std::round(box.origin.y + row * ch); };
    ImDrawList *dl = box.drawList;
    dl->AddRect(ImVec2(px(1.5f), py(0.5f)), ImVec2(px(w - 1.5f) + 1, py(h - 0.5f) + 1), line);
    for (int i = 0; i < static_cast<int>(menu.items.size()); i++) {
        const MenuItemDef &item = menu.items[static_cast<size_t>(i)];
        const int y = i + 1;
        if (item.label.empty()) {
            dl->AddLine(ImVec2(px(1.5f), py(y + 0.5f)), ImVec2(px(w - 1.5f), py(y + 0.5f)), line);
            continue;
        }
        const bool selected = i == menuItem;
        const ThemeAttr &color = themeColor(selected ? ThemeColor::MenuSelected : ThemeColor::Menu);
        const ThemeAttr &key = themeColor(selected ? ThemeColor::MenuSelectedShortcut : ThemeColor::MenuShortcut);
        box.fill(2, y, w - 4, color);
        putTilde(box, 3, y, item.label, color, key);
        if (!item.key.empty()) {
            box.text(w - 3 - static_cast<int>(item.key.size()), y, item.key, color);
        }
    }
}

// The status line (Turbo Vision's TStatusLine): " ~F1~ Help " ... from column 0; while a command is typed, the
// help line (SoftICE): the matching commands, what the command does, its syntax
void DebuggerApp::drawStatusLine()
{
    const int cols = static_cast<int>(screenW / cw) + 1;
    Canvas line(ImGui::GetForegroundDrawList(), ImVec2(0, screenH - ch), ImVec2(cw, ch), cols, 1);
    const ThemeAttr &normal = themeColor(ThemeColor::Menu);
    line.fill(0, 0, cols, normal);
    std::string text = sizing ? std::string("Move: arrow keys  Size: Shift+arrow keys  Done: Enter") : hint;
    if (!text.empty()) {
        // too long: cut a list after the last item that fits and add "...", as SoftICE does
        const size_t room = static_cast<size_t>(std::max(0, cols - 3));
        if (text.size() > room) {
            size_t cut = (room >= 5) ? text.rfind(", ", room - 5) : std::string::npos;
            text = (cut != std::string::npos) ? text.substr(0, cut) + ", ..." : text.substr(0, room);
        }
        line.text(1, 0, text, normal);
        return;
    }
    int i = 0;
    for (const char *item : kStatusItems) {
        int l = static_cast<int>(plain(item).size());
        if (i + l < cols) {
            line.text(i, 0, " ", normal);
            putTilde(line, i + 1, 0, item, normal, themeColor(ThemeColor::MenuShortcut));
            line.text(i + l + 1, 0, " ", normal);
        }
        i += l + 2;
    }
}

void DebuggerApp::openMenu(int index)
{
    if (menuOpen < 0) {
        focusBeforeMenu = focused();
    }
    menuOpen = index;
    menuItem = 0;
    std::vector<MenuDef> list = menus();
    while (menuItem < static_cast<int>(list[static_cast<size_t>(index)].items.size()) &&
           list[static_cast<size_t>(index)].items[static_cast<size_t>(menuItem)].label.empty()) {
        menuItem++;
    }
}

void DebuggerApp::closeMenu()
{
    menuOpen = -1;
    if (focusBeforeMenu != 0 && pane(focusBeforeMenu).open) {
        select(focusBeforeMenu); // (clicks on the menus took the focus from it)
    }
}

namespace {
// While the game runs, the only commands are the ones that stop it (SoftICE): F5 / Ctrl+D, F8, F10, F12, Alt+X
bool stopsTheGame(int command)
{
    switch (command) {
    case DebuggerApp::cmGo:
    case DebuggerApp::cmBreak:
    case DebuggerApp::cmTrace:
    case DebuggerApp::cmStepOver:
    case DebuggerApp::cmStepReturn:
    case DebuggerApp::cmQuit: return true;
    default: return false;
    }
}
} // namespace

// The keys and the mouse for the debugger: menus, status line, function keys, then the window with the keyboard and
// the command line. While the game runs, only the keys that stop it. (The status line does not take the mouse.)
void DebuggerApp::handleInput()
{
    ImGuiIO &io = ImGui::GetIO();
    const bool running = Emulator::instance().running();
    // typing that arrived while the game ran (it reaches this frame after the stop): dropped too
    const bool typingAllowed = !running && !ranLastFrame;
    ranLastFrame = running;
    if (running) {
        sizing = false;
        if (menuOpen >= 0) {
            closeMenu(); // (the game window's F5 started it)
        }
    }
    if (menuOpen >= 0) {
        menuInput();
        return;
    }
    if (sizing) {
        sizeInput();
        return;
    }
    // the mouse on the menu bar: a menu; anywhere else outside the windows (the status line too): nothing
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
        const int x = static_cast<int>(io.MousePos.x / cw);
        if (io.MousePos.y >= 0 && io.MousePos.y < ch && !running) {
            std::vector<MenuDef> list = menus();
            for (size_t i = 0; i < list.size() && i < std::size(menuX); i++) {
                int l = static_cast<int>(plain(list[i].title).size());
                if (x >= menuX[i] && x < menuX[i] + l + 2) {
                    focusBeforeMenu = focusedPane;
                    openMenu(static_cast<int>(i));
                    return;
                }
            }
        }
        select(focusedPane); // (ImGui took the focus away for a click outside the windows: give it back)
    }

    const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
    // Alt+letter: a menu (Alt+X: exit)
    if (alt && !ctrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            defer([this] { doCommand(cmQuit); });
            return;
        }
        std::vector<MenuDef> list = menus();
        for (size_t i = 0; i < list.size() && !running; i++) {
            char letter = hotkey(list[i].title);
            if (letter >= 'a' && letter <= 'z' &&
                ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A + (letter - 'a')), false)) {
                openMenu(static_cast<int>(i));
                return;
            }
        }
    }
    // the function keys (as the status line and the menus show them)
    struct FunctionKey {
        ImGuiKey key;
        bool ctrl, shift, repeat;
        int command;
    };
    static const FunctionKey kKeys[] = {
        {ImGuiKey_F1, false, false, false, cmHelpCommands}, {ImGuiKey_F3, false, false, false, cmOpenRom},
        {ImGuiKey_F4, false, false, false, cmShowScreen},   {ImGuiKey_F5, false, false, false, cmGo},
        {ImGuiKey_F6, false, false, false, cmEnterCode},    {ImGuiKey_F7, false, false, false, cmHere},
        {ImGuiKey_F8, false, false, true, cmTrace},         {ImGuiKey_F9, false, false, false, cmToggleBreakpoint},
        {ImGuiKey_F10, false, false, true, cmStepOver},     {ImGuiKey_F12, false, false, true, cmStepReturn},
        {ImGuiKey_F12, false, true, false, cmScreenshot},   {ImGuiKey_F10, false, true, false, cmFps},
        {ImGuiKey_D, true, false, false, cmBreak},          {ImGuiKey_F5, true, false, false, cmZoom},
        {ImGuiKey_F6, true, false, false, cmNext},          {ImGuiKey_F7, true, false, false, cmResize},
        {ImGuiKey_Equal, true, false, true, cmFontLarger},  {ImGuiKey_KeypadAdd, true, false, true, cmFontLarger},
        {ImGuiKey_Minus, true, false, true, cmFontSmaller}, {ImGuiKey_KeypadSubtract, true, false, true, cmFontSmaller},
    };
    if (!alt) {
        for (const FunctionKey &k : kKeys) {
            if (k.ctrl == ctrl && (k.shift == shift || (k.key == ImGuiKey_Equal && ctrl)) &&
                ImGui::IsKeyPressed(k.key, k.repeat) && (!running || stopsTheGame(k.command))) {
                int command = k.command;
                defer([this, command] { doCommand(command); });
            }
        }
    }
    if (!typingAllowed) {
        return;
    }

    // other keys: the window with the keyboard, then the command line (SoftICE: typing goes to the command line)
    static const ImGuiKey kEditKeys[] = {
        ImGuiKey_UpArrow,   ImGuiKey_DownArrow, ImGuiKey_LeftArrow,   ImGuiKey_RightArrow, ImGuiKey_PageUp,
        ImGuiKey_PageDown,  ImGuiKey_Home,      ImGuiKey_End,         ImGuiKey_Insert,     ImGuiKey_Delete,
        ImGuiKey_Backspace, ImGuiKey_Enter,     ImGuiKey_KeypadEnter, ImGuiKey_Tab,        ImGuiKey_Escape,
    };
    for (ImGuiKey key : kEditKeys) {
        if (!ImGui::IsKeyPressed(key, true)) {
            continue;
        }
        if (key == ImGuiKey_Insert && shift && !ctrl) {
            paste();
            continue;
        }
        if (key == ImGuiKey_Insert && ctrl && !shift) {
            copy();
            continue;
        }
        KeyPress press;
        press.key = key;
        press.ctrl = ctrl;
        press.shift = shift;
        press.alt = alt;
        dispatchKey(press, key == ImGuiKey_Tab); // (Tab completes the command line, whichever window has the focus)
    }
    if (ctrl && !alt) {
        for (int k = ImGuiKey_A; k <= ImGuiKey_Z; k++) {
            if (k == ImGuiKey_D || !ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), true)) {
                continue; // (Ctrl+D: Break)
            }
            if (k == ImGuiKey_V) {
                paste();
                continue;
            }
            if (k == ImGuiKey_C) {
                copy();
                continue;
            }
            KeyPress press;
            press.key = static_cast<ImGuiKey>(k);
            press.ctrl = true;
            press.shift = shift;
            dispatchKey(press, false);
        }
    }
    if (!ctrl && !alt) {
        for (ImWchar c : io.InputQueueCharacters) {
            if (c >= 32 && c < 127) {
                KeyPress press;
                press.ch = static_cast<char>(c);
                dispatchKey(press, false);
            }
        }
    }
}

// A key for the window with the keyboard; what it does not use goes to the command line (not while the game runs)
void DebuggerApp::dispatchKey(const KeyPress &key, bool commandLineFirst)
{
    View *view = focusedView();
    if (Emulator::instance().running()) {
        return; // (handleInput() sends none: only the keys that stop the game work while it runs)
    }
    if (!commandLineFirst && view != nullptr && view->key(key)) {
        return;
    }
    if (cmdView == nullptr) {
        openCommand();
    }
    if (cmdView != nullptr && (view != cmdView || commandLineFirst)) {
        cmdView->key(key);
    }
}

// Ctrl+C / Ctrl+Insert: the selection of the window with the keyboard (the code window), else of the command
// window, to the clipboard
void DebuggerApp::copy()
{
    View *view = focusedView();
    std::string text = (view != nullptr) ? view->selectedText() : std::string();
    if (text.empty() && cmdView != nullptr) {
        text = cmdView->selectedText();
    }
    if (!text.empty()) {
        SDL_SetClipboardText(text.c_str());
    }
}

// The middle button: the X11 primary selection (what was last selected, here or in another program)
void DebuggerApp::pastePrimary()
{
#if SDL_VERSION_ATLEAST(2, 26, 0)
    char *text = SDL_GetPrimarySelectionText();
    if (text != nullptr) {
        std::string s(text);
        SDL_free(text);
        s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
        if (!s.empty() && cmdView != nullptr) {
            cmdView->paste(s);
        }
    }
#endif
}

void DebuggerApp::paste()
{
    char *text = SDL_GetClipboardText();
    if (text != nullptr) {
        if (cmdView == nullptr) {
            openCommand();
        }
        std::string s(text);
        s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
        cmdView->paste(s);
        SDL_free(text);
    }
}

// The keys and the mouse while a menu is open: Left/Right the next menu, Up/Down the item, Enter or the item's
// letter chooses it, Esc closes; Alt+letter another menu
void DebuggerApp::menuInput()
{
    ImGuiIO &io = ImGui::GetIO();
    std::vector<MenuDef> list = menus();
    if (menuOpen >= static_cast<int>(list.size())) {
        closeMenu();
        return;
    }
    const std::vector<MenuItemDef> &items = list[static_cast<size_t>(menuOpen)].items;
    const int count = static_cast<int>(items.size());
    auto choose = [&](int index) {
        int command = items[static_cast<size_t>(index)].command;
        closeMenu();
        defer([this, command] { doCommand(command); });
    };
    auto step = [&](int delta) {
        for (int n = 0; n < count; n++) {
            menuItem = (menuItem + delta + count) % count;
            if (!items[static_cast<size_t>(menuItem)].label.empty()) {
                return;
            }
        }
    };

    // the mouse: titles on the menu bar, items in the menu
    std::vector<std::string> labels, keys;
    for (const MenuItemDef &item : items) {
        labels.push_back(item.label);
        keys.push_back(item.key);
    }
    const int w = menuWidth(labels, keys);
    const int boxX = std::max(0, std::min(menuX[menuOpen], static_cast<int>(screenW / cw) - w));
    const int mx = static_cast<int>(std::floor(io.MousePos.x / cw)),
              my = static_cast<int>(std::floor(io.MousePos.y / ch));
    const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (my == 0) {
        for (size_t i = 0; i < list.size() && i < std::size(menuX); i++) {
            int l = static_cast<int>(plain(list[i].title).size());
            if (mx >= menuX[i] && mx < menuX[i] + l + 2) {
                if (clicked && static_cast<int>(i) == menuOpen) {
                    closeMenu(); // (its title again: closed)
                    return;
                }
                if ((moved || clicked) && static_cast<int>(i) != menuOpen) {
                    openMenu(static_cast<int>(i));
                    return;
                }
            }
        }
    } else if (mx > boxX && mx < boxX + w - 1 && my >= 2 && my < 2 + count) {
        int index = my - 2;
        if (!items[static_cast<size_t>(index)].label.empty()) {
            if (moved || clicked) {
                menuItem = index;
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && index == menuItem) {
                choose(index);
                return;
            }
        }
    } else if (clicked) {
        closeMenu(); // (a click outside the menu)
        return;
    }

    // the keys
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        closeMenu();
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
        openMenu((menuOpen + static_cast<int>(list.size()) - 1) % static_cast<int>(list.size()));
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
        openMenu((menuOpen + 1) % static_cast<int>(list.size()));
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
        step(-1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
        step(1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
        menuItem = count - 1;
        step(1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
        menuItem = 0;
        step(-1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        if (menuItem >= 0 && menuItem < count && !items[static_cast<size_t>(menuItem)].label.empty()) {
            choose(menuItem);
        }
        return;
    }
    if (io.KeyAlt) {
        for (size_t i = 0; i < list.size(); i++) {
            char letter = hotkey(list[i].title);
            if (letter >= 'a' && letter <= 'z' &&
                ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A + (letter - 'a')), false)) {
                openMenu(static_cast<int>(i));
                return;
            }
        }
        return;
    }
    for (ImWchar c : io.InputQueueCharacters) {
        if (c >= 128) {
            continue;
        }
        char letter = static_cast<char>(std::tolower(static_cast<int>(c)));
        for (int i = 0; i < count; i++) {
            if (!items[static_cast<size_t>(i)].label.empty() && hotkey(items[static_cast<size_t>(i)].label) == letter) {
                choose(i);
                return;
            }
        }
    }
}

// Ctrl+F7 (Size/move): the arrow keys move the window with the keyboard by a character, Shift+arrows size it;
// Enter or Esc ends it
void DebuggerApp::sizeInput()
{
    PaneId id = focused();
    if (id == kNoPane || !pane(id).open) {
        sizing = false;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        sizing = false;
        return;
    }
    const bool shift = ImGui::GetIO().KeyShift;
    int dx = 0, dy = 0;
    dx -= ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) ? 1 : 0;
    dx += ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) ? 1 : 0;
    dy -= ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) ? 1 : 0;
    dy += ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) ? 1 : 0;
    if (dx == 0 && dy == 0) {
        return;
    }
    Cells r = cellsOf(pane(id));
    if (shift) {
        r.bx = std::max(r.ax + 16, r.bx + dx);
        r.by = std::max(r.ay + 3, r.by + dy);
    } else {
        const Cells d = desktop();
        int nx = std::clamp(r.ax + dx, -(r.bx - r.ax) + 2, d.bx - 2), ny = std::clamp(r.ay + dy, 0, d.by - 1);
        r.bx += nx - r.ax;
        r.by += ny - r.ay;
        r.ax = nx;
        r.ay = ny;
    }
    place(id, r);
    pane(id).zoomed = false;
}

// --- commands ----------------------------------------------------------------------------------------------------

void DebuggerApp::toggleBreakpointAtCursor()
{
    Emulator &emu = Emulator::instance();
    uint16_t address = (codeView && focused() == kCode) ? codeView->cursorAddress() : cpu_debug_get_pc();
    int slot = emu.breakpointAt(address);
    if (slot >= 0) {
        emu.removeBreakpoint(static_cast<unsigned>(slot));
    } else if (emu.addBreakpoint(Breakpoint::Type::Execute, address) < 0) {
        print("Cannot set breakpoint: all 16 are in use");
    }
}

void DebuggerApp::doCommand(int command)
{
    Emulator &emu = Emulator::instance();
    bool running = emu.running();
    if (!emu.hasRom()) { // the windows that show the game, and the keys that act on it, need one
        switch (command) {
        case cmToggleBreakpoint:
        case cmWinRegisters:
        case cmWinPpu:
        case cmWinCode:
        case cmWinCpuMemory:
        case cmWinPpuMemory:
        case cmWinOam:
        case cmWinWatch:
        case cmEnterCode: print("No game loaded: File > Open game (F3)"); return;
        default: break;
        }
    }
    switch (command) {
    case cmGo:
        if (running) {
            emu.stop();
        } else {
            commands.go();
        }
        break;
    case cmBreak:
        if (running) {
            emu.stop();
        }
        break;
    // F8 / F10 / F12 while the game runs: stop at the next instruction, then step from there (as in the game window)
    case cmTrace:
    case cmStepOver:
    case cmStepReturn:
        if (running) {
            emu.stop();
            afterStop = command; // (idle(): once it has stopped)
        } else if (command == cmTrace) {
            commands.stepInto(1);
        } else if (command == cmStepOver) {
            commands.stepOver();
        } else {
            commands.stepToReturn();
        }
        break;
    case cmToggleBreakpoint: toggleBreakpointAtCursor(); break;
    case cmShowScreen: commands.execute("rs"); break;
    case cmHelpCommands: commands.execute("help"); break;
    case cmReset:
        if (!running)
            commands.execute("reset");
        break;
    case cmPowerCycle:
        if (!running)
            commands.execute("reset hard");
        break;
    case cmWinRegisters:
        openRegisters();
        stopped();
        break;
    case cmWinPpu:
        openPpu();
        stopped();
        break;
    case cmWinCode: openCode(); break;
    case cmWinCpuMemory: openCpuMemory(); break;
    case cmWinPpuMemory: openPpuMemory(); break;
    case cmWinOam: openOam(); break;
    case cmWinWatch:
        openWatches();
        select(kWatch);
        break;
    case cmWinCommand: openCommand(); break;
    case cmDefaultLayout: defaultLayout(); break;
    case cmEnterCode: toggleCodeFocus(); break;
    case cmScreenshot: commands.execute("screenshot"); break; // (also while running: the last complete frame)
    case cmFps: commands.execute("fps"); break;
    case cmLuaRun: runLuaScript(); break;
    case cmOpenRom:
        if (!running)
            openRomDialog();
        break;
    case cmLuaStop: commands.execute("lua stop"); break;
    case cmLuaList: commands.execute("lua"); break;
    case cmQuickReference: print(openQuickReference()); break;
    case cmHere:
        if (!running)
            commands.execute("here");
        break;
    case cmQuit: quit(); break;
    case cmNext: nextWindow(); break;
    case cmZoom: zoomWindow(); break;
    case cmResize: sizing = focused() != kNoPane; break;
    case cmTile: tileWindows(); break;
    case cmCascade: cascadeWindows(); break;
    case cmFontLarger: changeFontSize(2); break;
    case cmFontSmaller: changeFontSize(-2); break;
    default:
        if (command >= cmRecentRom && command <= cmRecentRomLast && !running) {
            openCommand();
            commands.execute(std::string("load \"") + fe_recent(FE_RECENT_GAMES, command - cmRecentRom) + "\"");
        }
        break;
    }
}

std::string DebuggerApp::chooseFile(const std::string &dir, const char *wildcard, const char *title)
{
    FileDialog dialog(title, dir, wildcard);
    runDialog(dialog);
    return dialog.chosen;
}

// Lua > Run script...: a file dialog (*.lua), starting where the last script was chosen, else in the ROM's
// directory; the chosen file goes to the lua command (so it is in the command window's history too)
void DebuggerApp::runLuaScript()
{
    static std::string lastDir;
    std::string name = chooseFile(lastDir.empty() ? directoryOf(fe_rom_path()) : lastDir, "*.lua", "Run a Lua script");
    if (!name.empty()) {
        lastDir = directoryOf(name);
        openCommand();
        commands.execute("lua \"" + name + "\"");
    }
}

// File > Open game...: a file dialog (*.nes), starting where the last game was chosen, else in the directory of the
// game loaded or of the latest recent one; the chosen file goes to the load command
void DebuggerApp::openRomDialog()
{
    static std::string lastDir;
    std::string dir = lastDir;
    if (dir.empty()) {
        dir = directoryOf(Emulator::instance().hasRom() ? Emulator::instance().romPath()
                                                        : std::string(fe_recent(FE_RECENT_GAMES, 0)));
    }
    std::string name = chooseFile(dir, "*.nes;*.zip", "Open a game");
    if (!name.empty()) {
        lastDir = directoryOf(name);
        openCommand();
        commands.execute("load \"" + name + "\"");
    }
}

// The game window has the keyboard focus but the keys that stop the game work there as well: F5 go/stop, F8 trace,
// F10 step over, F12 step to return, Ctrl+D break, Alt+X quit. Pressed while the game runs, the key first stops it at
// the next instruction; F8 therefore stops, executes one instruction and stops again at the one after it. (The
// other keys of the game window are its own: F1/F2 save states, Shift+F12 screenshot, ...; input.c.)
void DebuggerApp::gameKey(const GameKey &key)
{
    if (key.key == GameKey::kQuit) {
        print("Game window closed");
        quit(); // asks first if labels are unsaved
        return;
    }
    if (key.key == GameKey::kAltX) {
        print("Alt+X in the game window");
        quit();
        return;
    }
    int command = 0;
    const char *name = nullptr;
    switch (key.key) {
    case 5:
        command = cmGo;
        name = "F5";
        break;
    case 8:
        command = cmTrace;
        name = "F8";
        break;
    case 10:
        command = cmStepOver;
        name = "F10";
        break;
    case 12:
        command = cmStepReturn;
        name = "F12";
        break;
    case GameKey::kCtrlD:
        command = cmBreak;
        name = "Ctrl+D";
        break;
    default: return;
    }
    if (key.whileRunning) {
        print(std::string(name) + " in the game window");
        if (command == cmGo || command == cmBreak) {
            return; // stopping was the whole job
        }
    }
    doCommand(command);
}

// Quitting with labels that were added, renamed or removed since the last save (n, lsave, lload): offer to save.
bool DebuggerApp::confirmQuit()
{
    if (!symbols.modified()) {
        return true;
    }
    std::string file = symbols.fileName();
    std::string text = "The labels or comments have unsaved changes.\n\nSave them" +
                       (file.empty() ? std::string("?") : " to " + file + "?");
    MessageDialog question("Warning", text, {"~Y~es", "~N~o", "~C~ancel"});
    runDialog(question);
    if (question.answer == 1) {
        return true;
    }
    if (question.answer != 0) {
        return false; // Cancel / Esc: keep debugging
    }
    if (file.empty()) {
        InputDialog ask("Save labels", "~F~ile name", "labels.txt");
        runDialog(ask);
        if (!ask.ok || ask.value.empty()) {
            return false;
        }
        file = ask.value;
    }
    if (!symbols.save(file)) {
        MessageDialog failed("Error", "Cannot write " + file, {"OK"});
        runDialog(failed);
        return false;
    }
    symbols.setFileName(file);
    symbols.setModified(false);
    return true;
}
