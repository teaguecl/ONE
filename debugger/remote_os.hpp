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
// The remote-control channel: see remote.hpp.
// What the remote channel (remote.cpp) needs from the operating system: its sockets and files. remote_posix.cpp
// (Unix socket or TCP, Linux) and remote_win.cpp (TCP on 127.0.0.1, Windows) implement it; CMake picks one.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace remote_os {

using Socket = std::intptr_t;
constexpr Socket kNoSocket = -1;

// The listening socket and the files that go with it
struct Channel {
    Socket listener = kNoSocket;
    bool local = false;     // a Unix socket (the peer is checked), else TCP on 127.0.0.1
    std::string dir;        // the private directory of the token, socket and screenshot files
    std::string socketPath; // the Unix socket; on Windows the PID.port file that says where the TCP port is
    std::string tokenPath;
};

// Make the private directory, remove the files of debuggers that ended, write a new random TOKEN to a file in it,
// and listen: PORT < 0 the default (a Unix socket where there is one, else TCP on a free port), 0 TCP on a free
// port, else TCP on that port. LISTENING: "listening on <socket or 127.0.0.1:PORT>, token <file>". False: ERROR.
bool openChannel(int port, Channel &channel, std::string &token, std::string &listening, std::string &error);
void closeChannel(Channel &channel); // close the socket, delete the files

// --batch: on a termination signal delete the files and end. ALWAYS: replace a handler that is there already
void handleSignals(bool always);

bool realPath(const std::string &path, std::string &real); // as the ROM directories are compared (false: no such)
long processId();

// The client's socket: accepted (kNoSocket: none waiting), and checked to be this user's (a Unix socket's peer)
Socket acceptClient(Socket listener);
bool peerIsThisUser(Socket client, const Channel &channel);
void configureClient(Socket client); // a send that cannot finish for 5 seconds fails
void closeSocket(Socket socket);
void sendBestEffort(Socket socket, const std::string &text); // without waiting, for a refusal

// >0 bytes read, 0 the peer went away (or the socket failed), -1 nothing to read now
std::ptrdiff_t receive(Socket client, char *buffer, std::size_t size);
// >0 bytes sent, <=0 failed; waits up to the time configureClient() set
std::ptrdiff_t sendSome(Socket client, const char *data, std::size_t size);
void waitForInput(Socket listener, Socket client, int milliseconds); // client may be kNoSocket

} // namespace remote_os
