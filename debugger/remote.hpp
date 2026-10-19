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
// The remote-control channel, for the MCP adapter (tools/mcp/): with
// --remote [PORT], one-debugger takes JSON requests, one per line, from one client at a time, on a Unix socket in
// $XDG_RUNTIME_DIR/one-debugger (or TCP on 127.0.0.1), once the client has sent the token from the token file next to
// it. Only allowlisted commands run: nothing that runs Lua or reads or writes a file the client names.
//
// Requests (each reply is one line with the request's "id", "ok", and "error" or the results, and "state"):
//   {"auth": "<token>"}                     must come first
//   {"id": 1, "cmd": "d 300"}               a debugger command (only while the emulation is stopped): "output". A
//                                           command that runs the game (fs 10, x, g...) replies when it stops, or
//                                           after "timeout" seconds (default 10) with "timed_out": true
//   {"id": 2, "op": "pause"}                stop the emulation (also while another request waits for a stop)
//   {"id": 3, "op": "wait", "timeout": 60}  until the emulation stops (a breakpoint, the user's break key)
//   {"id": 4, "op": "screenshot"}           the last complete frame: "png" (base64)
//   {"id": 5, "op": "state"}                only the state: running, pc, frame, scanline, cycle, rom
//   {"id": 6, "op": "read", "space": "cpu", "address": 768, "length": 16}   memory as "data" (hex); space: cpu
//                                           (side-effect free; Game Genie codes included), ppu, oam, palette
//   {"id": 7, "op": "disassemble", "address": 49152, "count": 16}  "lines": the code there, with labels
//   {"id": 6, "op": "read", "bank": 5, "address": 47799, "length": 3}  PRG-ROM bank 5 as it shows at $BAB7 when
//                                           mapped in (also when it is not); "space": "prg" without a bank: by ROM
//                                           offset. disassemble takes a bank too.
//   {"id": 7, "op": "rominfo"}              PRG-ROM: prg_size, bank_size (0: no PRG banking), banks, and "windows":
//                                           which bank each window of $8000-$FFFF shows now ("8000=5 C000=7")
//   {"id": 8, "op": "notify", "message": "..."}  a message to the player: the command window and the game window
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "remote_os.hpp"

class CommandProcessor;

class RemoteServer {
public:
    static RemoteServer &instance();
    RemoteServer(const RemoteServer &) = delete;
    RemoteServer &operator=(const RemoteServer &) = delete;

    // PORT < 0: the Unix socket (Windows: TCP on a free port); 0: TCP on a free port. ROM_DIRS: the directories 'load' may load games from.
    // LISTENING: "listening on <socket or 127.0.0.1:PORT>, token <file>". False: ERROR says why.
    bool start(int port, const std::vector<std::string> &romDirs, std::string &listening, std::string &error);
    void shutDown(); // close the connections, delete the socket and token files
    bool active() const { return channel.listener >= 0; }
    // --batch: on SIGTERM / SIGINT / SIGHUP delete the files and end (the UIs leave those signals to SDL)
    static void handleSignals(bool always);

    // From the host's loop, between slices: connect, read, answer. Commands run while the emulation is stopped and no
    // other command is waiting (the -s script, a breakpoint's actions).
    void poll(CommandProcessor &commands);
    void waitForInput(int milliseconds) const; // --batch, nothing to do: until a client connects or writes

    // The allowlist: "" if LINE may run over the channel, else why not
    static std::string refusal(const std::string &line);
    // Breakpoint conditions (Lua, with all its libraries): only registers, mem[...], numbers and operators
    static std::string conditionRefusal(const std::string &expression);
    std::string romRefusal(const std::string &file) const; // load: only from the ROM directories

private:
    RemoteServer() = default;
    ~RemoteServer() { shutDown(); }
    struct Request {
        std::string line;
        bool command = false; // {"cmd": ...}: runs once nothing else is waiting to run
    };
    struct Waiting {    // a request waiting for the emulation to stop
        std::string id; // (its JSON text)
        double deadline = 0;
        bool active = false;
    };
    void accept();
    void read();
    void drop();
    void handle(CommandProcessor &commands, const std::string &line);
    void runCommand(CommandProcessor &commands, const std::string &id, const std::string &command, double timeout);
    void reply(const std::string &id, bool ok, const std::string &fields);
    void replyError(const std::string &id, const std::string &message) { reply(id, false, errorField(message)); }
    static std::string errorField(const std::string &message);
    std::string takeOutput(); // what the commands printed for the client since the last reply (a "output" field)
    static std::string state();
    static uint32_t prgBankSize();

    remote_os::Channel channel; // the listening socket and the files
    remote_os::Socket clientFd = remote_os::kNoSocket;
    bool authorized = false;
    double connectedAt = 0;
    std::string token;
    std::vector<std::string> romDirs; // (real paths)
    std::string input;                // received, not yet a whole line
    std::deque<Request> requests;
    Waiting waiting;
    std::string output;  // printed by the commands for the client (CommandProcessor's tee)
    bool teeing = false; // the output is collected (a run the client started has not ended yet)
    CommandProcessor *teeOf = nullptr;
};
