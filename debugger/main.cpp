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
// one-debugger: SoftICE-style debugger for the ONE NES emulator.
//
//   one-debugger [options] [ROM]   (without a ROM: File > Open game, or the load command)
//     -l, --labels FILE   load labels (Mesen .mlb, ld65 --dbgfile, ld65 -Ln / VICE); game.mlb is loaded anyway
//     -s, --script FILE   run debugger commands from FILE at start-up ('#' comments, '!' script commands)
//     -t, --trace         write the Mesen-format CPU trace to the log file
//     -u, --unthrottled   run as fast as possible instead of at NES speed
//     -H, --headless      no game window
//         --log FILE      where the emulator's own output goes (default one-debugger.log)
//         --config FILE   settings (default ./one-debugger.cfg or ~/.config/one/debugger.cfg; see theme.cpp)
//         --theme NAME    built-in colors: softice (default) or borland; overrides 'theme =' in the config
//         --write-config FILE   write a config file with every setting of the theme (--theme), then exit
//         --batch         no user interface: run the script, print to stdout, exit (scripted tests);
//                         the exit code is 1 if a '!assert' failed
//         --no-init       skip the ROM's command file (game.nes -> game.init), which otherwise runs first
//         --remote [PORT] take requests from the MCP adapter (remote.hpp): a Unix socket, or TCP on 127.0.0.1
//         --remote-roms DIR   where the remote channel's 'load' may load games from (repeatable; default: the
//                             start-up ROM's directory)
//
// The emulator core prints diagnostics on stdout/stderr, so both are redirected to the log file. The user
// interface is ImGui, a window of its own (debugger/imgui/).
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <io.h>
#define dup    _dup
#define fdopen _fdopen
#define fileno _fileno
#else
#include <cstdlib>
#include <unistd.h>
#endif

#include "imgui/app.hpp"
#include "viewer_window.hpp"
#include "commands.hpp"
#include "core.hpp"
#include "emulator.hpp"
#include "frontend.h" // (fe_recent_add())
#include "symbols.hpp"
#include "config.hpp"
#include "theme.hpp"
#include "settings.hpp"
#include "watches.hpp"
#include "window_places.hpp"
#include "remote.hpp"

namespace {

struct Options {
    std::string rom, labels, script, log = "one-debugger.log", config, writeConfig, theme, oneConfig;
    std::vector<std::string> lua; // --lua
    int remotePort = -2;          // --remote: -1 the Unix socket, else the TCP port (-2: no remote channel)
    std::vector<std::string> remoteRoms;
    bool trace = false, unthrottled = false, headless = false, batch = false, listGamepads = false;
    bool noInit = false;  // --no-init
    bool version = false; // --version
};

void usage()
{
    std::fprintf(
        stderr,
        "Usage: one-debugger [options] [ROM]   (no ROM: choose one in the debugger)\n"
        "  -l, --labels FILE   load labels: Mesen .mlb, ld65 --dbgfile or -Ln (the ROM's .mlb loads by itself)\n"
        "  -s, --script FILE   run debugger commands from FILE at start-up\n"
        "      --lua FILE      run a Lua script (Mesen 2 API), like the lua command; may be repeated\n"
        "  -t, --trace         write the Mesen-format CPU trace to the log file\n"
        "  -u, --unthrottled   run as fast as possible instead of at NES speed\n"
        "  -H, --headless      no game window\n"
        "      --log FILE      emulator output (default one-debugger.log)\n"
        "  -V, --version       print the version and exit\n"
        "      --config FILE   settings (default ./one-debugger.cfg, else ~/.config/one/debugger.cfg)\n"
        "      --one-config FILE  emulator settings (one.cfg: controllers, sound)\n"
        "      --list-gamepads  list the gamepads and show their button names\n"
        "      --theme NAME    built-in colors: softice (default) or borland\n"
        "      --write-config FILE  write a config file with every color of the theme (--theme), then exit\n"
        "      --batch         no user interface: run the script, print results to stdout, exit\n"
        "      --no-init       do not run the ROM's command file (game.nes -> game.init) at start-up\n"
        "      --remote [PORT] take requests from the MCP adapter: a Unix socket, or TCP on 127.0.0.1:PORT\n"
        "      --remote-roms DIR  where the remote channel may load games from (default: the ROM's directory)\n");
}

bool parseArgs(int argc, char **argv, Options &o)
{
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto value = [&](std::string &out) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                return false;
            }
            out = argv[++i];
            return true;
        };
        if (a == "-l" || a == "--labels") {
            if (!value(o.labels))
                return false;
        } else if (a == "-s" || a == "--script") {
            if (!value(o.script))
                return false;
        } else if (a == "--lua") {
            std::string file;
            if (!value(file))
                return false;
            o.lua.push_back(file);
        } else if (a == "--log") {
            if (!value(o.log))
                return false;
        } else if (a == "--config") {
            if (!value(o.config))
                return false;
        } else if (a == "--list-gamepads") {
            o.listGamepads = true;
        } else if (a == "--one-config") {
            if (!value(o.oneConfig))
                return false;
        } else if (a == "--write-config") {
            if (!value(o.writeConfig))
                return false;
        } else if (a == "--theme") {
            if (!value(o.theme))
                return false;
        } else if (a == "-t" || a == "--trace")
            o.trace = true;
        else if (a == "-u" || a == "--unthrottled")
            o.unthrottled = true;
        else if (a == "-H" || a == "--headless")
            o.headless = true;
        else if (a == "--batch")
            o.batch = true;
        else if (a == "--no-init")
            o.noInit = true;
        else if (a == "--remote") {
            o.remotePort = -1;
            if (i + 1 < argc && argv[i + 1][0] != '\0' &&
                std::strspn(argv[i + 1], "0123456789") == std::strlen(argv[i + 1])) {
                long port = std::strtol(argv[++i], nullptr, 10);
                if (port > 65535) {
                    std::fprintf(stderr, "--remote: no port %ld\n", port);
                    return false;
                }
                o.remotePort = static_cast<int>(port);
            }
        } else if (a == "--remote-roms") {
            std::string dir;
            if (!value(dir))
                return false;
            o.remoteRoms.push_back(dir);
        } else if (a == "-V" || a == "--version")
            o.version = true;
        else if (a == "-h" || a == "--help")
            return false;
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return false;
        } else
            o.rom = a;
    }
    return true; // (no ROM: the debugger asks for one)
}

// --batch: commands from the script, results on (the real) stdout
class BatchHost : public CommandHost {
public:
    explicit BatchHost(FILE *out) : out(out) {}
    void print(const std::string &line) override
    {
        std::fprintf(out, "%s\n", line.c_str());
        std::fflush(out);
    }
    void quit() override { quitRequested = true; }
    bool quitRequested = false;

private:
    FILE *out;
};

int runBatch(Symbols &symbols, std::deque<std::string> script, FILE *out, const std::vector<std::string> &messages)
{
    BatchHost host(out);
    for (const std::string &message : messages) {
        host.print(message);
    }
    CommandProcessor commands(host, symbols);
    // messages up to info level: in the results, like the UI's command window (all of them: the log file)
    log_set_hook(
        [](log_category category, log_level level, const char *message, void *context) {
            if (level <= LOG_LEVEL_WARN || (category == LOG_CAT_SCRIPT && level <= LOG_LEVEL_INFO)) {
                static_cast<BatchHost *>(context)->print(message); // (and the scripts' print())
            }
        },
        &host);
    Emulator &emu = Emulator::instance();
    commands.enqueue(std::vector<std::string>(script.begin(), script.end()));
    while (commands.hasPending() && !host.quitRequested) {
        commands.execute(commands.takePending());
        while (emu.running()) {
            StopReason reason = emu.runSlice();
            if (reason != StopReason::None) {
                commands.reportStop(reason);
            }
        }
    }
    // --remote: then the client's requests, until q (or a signal)
    RemoteServer &remote = RemoteServer::instance();
    while (remote.active() && !host.quitRequested) {
        remote.poll(commands);
        if (emu.running()) {
            StopReason reason = emu.runSlice();
            if (reason != StopReason::None) {
                commands.reportStop(reason);
            }
        } else if (commands.hasPending()) {
            commands.execute(commands.takePending());
        } else {
            remote.waitForInput(20);
        }
    }
    if (symbols.modified()) {
        host.print("warning: the labels or comments have unsaved changes (lsave saves them)");
    }
    log_set_hook(nullptr, nullptr); // host goes away
    return commands.assertionFailed() ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!parseArgs(argc, argv, options)) {
        usage();
        return 2;
    }
    if (options.version) {
        std::printf("ONE Debugger %s\n", one_version());
        return 0;
    }

    Theme::Preset preset = Theme::Preset::SoftIce;
    if (!options.theme.empty() && !Theme::presetFromName(options.theme, preset)) {
        std::fprintf(stderr, "unknown theme '%s' (softice, borland)\n", options.theme.c_str());
        return 2;
    }

    if (options.listGamepads) {
        return input_list_gamepads();
    }
    if (!options.writeConfig.empty()) {
        std::ofstream out(options.writeConfig);
        out << Theme::defaultConfigText(preset);
        if (!out) {
            std::fprintf(stderr, "cannot write '%s'\n", options.writeConfig.c_str());
            return 2;
        }
        std::printf("wrote %s\n", options.writeConfig.c_str());
        return 0;
    }

    // Settings: problems are reported in the command window, but don't stop the debugger
    std::vector<std::string> messages;
    std::string configFile = options.config.empty() ? Config::findDefault() : options.config;
    if (!configFile.empty()) {
        Config config;
        if (!config.load(configFile, messages)) {
            std::fprintf(stderr, "cannot open config file '%s'\n", configFile.c_str());
            return 2;
        }
        // the built-in theme first ('theme =' in the file, unless --theme was given), then the file's colors
        if (const std::string *name = config.get("theme"); name && options.theme.empty()) {
            if (!Theme::presetFromName(*name, preset)) {
                messages.push_back(configFile + ":" + std::to_string(config.line("theme")) + ": unknown theme '" +
                                   *name + "' (softice, borland)");
            }
        }
        Theme::instance().usePreset(preset);
        Theme::instance().apply(config, messages);
        applySettings(config, messages);
        for (const std::string &key : config.unusedKeys()) {
            std::string message = configFile;
            message += ":" + std::to_string(config.line(key)) + ": unknown setting '" + key + "'";
            messages.push_back(message);
        }
        messages.push_back("Settings from " + configFile);
    }

    if (configFile.empty()) {
        Theme::instance().usePreset(preset);
    }

    // Emulator settings (one.cfg, the same file as for one): controllers, sound, messages. Problems go to the command
    // window (the log file is not open yet, and the terminal is about to be taken over by the UI).
    log_set_console(false);
    log_set_hook([](log_category, log_level, const char *message,
                    void *context) { static_cast<std::vector<std::string> *>(context)->emplace_back(message); },
                 &messages);
    video_set_debugger(true); // overscan: the whole picture by default, or video.debugger_overscan
    if (settings_load(options.oneConfig.empty() ? nullptr : options.oneConfig.c_str()) < 0) {
        std::fprintf(stderr, "cannot open settings file '%s'\n", options.oneConfig.c_str());
        return 2;
    }
    log_set_hook(nullptr, nullptr);
    {
        bool noSpriteLimit = false;
        settings_parse_bool(settings_get("video.disable_sprite_limit"), &noSpriteLimit);
        if (noSpriteLimit && !ppu_sprite_limit_forced()) {
            messages.emplace_back(
                "warning: Disable Sprite Limit is on (video.disable_sprite_limit): the picture differs "
                "from the hardware's (traces with -t are not affected)");
        }
    }

    std::deque<std::string> script;
    if (!options.script.empty()) {
        std::ifstream in(options.script);
        if (!in) {
            std::fprintf(stderr, "cannot open script '%s'\n", options.script.c_str());
            return 2;
        }
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            script.push_back(line);
        }
    }

    // --lua: the Lua scripts start before the -s commands, as 'lua FILE' commands (their messages go where the
    // commands' go)
    for (auto it = options.lua.rbegin(); it != options.lua.rend(); ++it) {
        script.push_front("lua " + *it);
    }
    // The ROM's own command file (game.nes -> game.init), like its labels and watches: first of all. Not with
    // --batch, whose results must not depend on what lies next to the ROM.
    CommandProcessor::initFiles = !options.batch && !options.noInit; // (load: the game's .init too)
    cheats_set_persistent(!options.batch); // Game Genie codes remembered per game (not in scripted runs)
    if (CommandProcessor::initFiles && !options.rom.empty()) {
        std::string init = options.rom;
        size_t initDot = init.find_last_of('.'), initSlash = init.find_last_of("/\\");
        if (initDot != std::string::npos && (initSlash == std::string::npos || initDot > initSlash)) {
            init.erase(initDot);
        }
        init += ".init";
        if (std::ifstream(init)) {
            messages.push_back("Commands from " + init);
            script.push_front("source " + init);
        }
    }

    // Labels: the ROM's own label file (game.nes -> game.mlb, Mesen's format) is loaded if it exists and is where
    // lsave writes; -l adds a file from the assembler (ld65 --dbgfile or -Ln) or another .mlb.
    Symbols symbols;
    std::string romLabels = options.rom;
    size_t dot = romLabels.find_last_of('.'), slash = romLabels.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        romLabels.erase(dot);
    }
    romLabels += ".mlb";
    if (!options.rom.empty()) {
        symbols.setFileName(romLabels);
    }
    auto labelMessage = [&symbols](int count, const std::string &file, const std::string &kind) {
        size_t comments = symbols.comments(); // (all of them so far: fine, the ROM's own file comes first)
        return std::to_string(count) + (count == 1 ? " label" : " labels") +
               (comments == 0 ? std::string()
                              : " and " + std::to_string(comments) + (comments == 1 ? " comment" : " comments")) +
               " from " + file + " (" + kind + ")";
    };
    if (!options.rom.empty() && std::ifstream(romLabels) && romLabels != options.labels) {
        std::string kind;
        int count = symbols.loadFile(romLabels, &kind);
        if (count >= 0) {
            messages.push_back(labelMessage(count, romLabels, kind));
        }
    }
    std::string romDbg = romLabels.substr(0, romLabels.size() - 4) + ".dbg"; // (ld65 --dbgfile next to the ROM)
    if (!options.rom.empty() && std::ifstream(romDbg) && romDbg != options.labels) {
        std::string kind;
        int count = symbols.loadFile(romDbg, &kind);
        if (count >= 0) {
            messages.push_back(labelMessage(count, romDbg, kind));
        }
    }
    if (!options.labels.empty()) {
        std::string kind;
        int count = symbols.loadFile(options.labels, &kind);
        if (count < 0) {
            std::fprintf(stderr, "cannot open label file '%s'\n", options.labels.c_str());
            return 2;
        }
        messages.push_back(labelMessage(count, options.labels, kind));
        if (options.labels.size() > 4 && options.labels.compare(options.labels.size() - 4, 4, ".mlb") == 0) {
            symbols.setFileName(options.labels); // lsave without a name writes it back
        }
    }

    // Keep the real stdout for --batch results, then send the emulator's own output to the log file.
    FILE *results = fdopen(dup(fileno(stdout)), "w");
    if (std::freopen(options.log.c_str(), "w", stdout) == nullptr ||
        std::freopen(options.log.c_str(), "a", stderr) == nullptr) {
        std::fprintf(results, "cannot open log file '%s'\n", options.log.c_str());
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 4096);
    std::setvbuf(stderr, nullptr, _IOFBF, 1 << 16);
    log_set_console(true); // all messages go to the log file from now on (stdout and stderr are the log file)
    std::fprintf(stderr, "ONE Debugger %s\n", one_version());
    if (options.trace) {
        cpu_set_trace(true);
    }

    video_init(options.headless || options.batch);
    video_set_title("ONE Debugger", "Stopped"); // (the game window says which program it belongs to, and its state)
    video_set_throttle(!options.unthrottled && !options.batch);
    if (!options.batch) {
        moveWindowTo(static_cast<SDL_Window *>(fe_sdl_window()), savedWindowPlace("game")); // (where it was last time)
    }
    input_init(); // gamepads (with the game window only)
    audio_init(video_is_headless());

    Emulator &emu = Emulator::instance();
    if (!options.rom.empty() && !emu.powerOn(options.rom)) {
        std::fprintf(results, "cannot load '%s' (see %s)\n", options.rom.c_str(), options.log.c_str());
        audio_shutdown();
        input_shutdown();
        video_shutdown();
        return 2;
    }

    if (!options.rom.empty() && !options.batch) { // (game.cdl: --batch scripts start from nothing)
        const std::string logMessage = CommandProcessor::loadSavedCodeLog();
        if (!logMessage.empty()) {
            messages.push_back(logMessage);
        }
    }

    // The remote channel: its socket and token file (the line tells the MCP adapter where they are)
    RemoteServer &remote = RemoteServer::instance();
    if (options.remotePort != -2) {
        std::vector<std::string> romDirs = options.remoteRoms;
        if (romDirs.empty() && !options.rom.empty()) {
            std::string romDir = options.rom.substr(0, options.rom.find_last_of("/\\") + 1);
            romDirs.push_back(romDir.empty() ? "." : romDir);
        }
        std::string listening, error;
        if (!remote.start(options.remotePort, romDirs, listening, error)) {
            std::fprintf(results, "remote: %s\n", error.c_str());
            emu_power_off();
            audio_shutdown();
            input_shutdown();
            video_shutdown();
            return 2;
        }
        std::fprintf(results, "remote: %s\n", listening.c_str());
        std::fflush(results);
        RemoteServer::handleSignals(options.batch); // (the UIs: only where SDL left the default)
    }

    int status;
    if (options.batch) {
        status = runBatch(symbols, std::move(script), results, messages);
    } else {
        // the watch list is kept next to the ROM (game.nes -> game.watch); --batch neither reads nor writes it
        if (!options.rom.empty()) {
            Watches::instance().setFileName(romLabels.substr(0, romLabels.size() - 4) + ".watch");
            fe_recent_add(FE_RECENT_GAMES, options.rom.c_str()); // (also with -H; the File menu lists them)
        }
        DebuggerApp app(symbols, std::move(script), messages);
        if (!app.startError().empty()) {
            std::fprintf(results, "%s\n", app.startError().c_str());
        }
        app.run();
        CommandProcessor::saveCodeLogIfOn();
        status = app.exitCode();
        saveWindowPlace("game", static_cast<SDL_Window *>(fe_sdl_window())); // (none with -H)
        log_set_hook(nullptr, nullptr);                                      // (the app queued them)
        ViewerWindow::closeAll(); // (wbg, wpal: they open there again next time)
        input_set_event_hook(nullptr, nullptr);
        app.shutDown();
    }

    remote.shutDown();
    emu_power_off();
    audio_shutdown();
    input_shutdown();
    video_shutdown();
    std::fflush(stdout);
    std::fflush(stderr);
    return status;
}
