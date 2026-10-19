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
// The remote channel's operating system layer on Windows (remote_os.hpp): TCP on 127.0.0.1, and the token file in
// %LOCALAPPDATA%\one-debugger (a directory only this user can read) and, next to it, PID.port: "127.0.0.1:PORT", for the
// MCP adapter to find a debugger that is running. There are no Unix sockets here, and the peer of a TCP connection
// cannot be checked: the token is what lets a client in.
// NOLINTBEGIN(misc-include-cleaner): the Windows API comes from <windows.h> and <winsock2.h> as a whole
#include "remote_os.hpp"

#include <winsock2.h> // (before windows.h)
#include <ws2tcpip.h>
#include <windows.h>

#include <bcrypt.h>

#include <cctype>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace remote_os {

namespace {

bool winsockStarted = false;

// For the signal handler: the files to delete
char signalToken[MAX_PATH], signalPort[MAX_PATH];

// A process of this number is running (a file of one that ended is stale)
bool processRuns(unsigned long pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return GetLastError() != ERROR_INVALID_PARAMETER; // (no such process; any other error: access, so it runs)
    }
    DWORD code = 0;
    const bool running = GetExitCodeProcess(process, &code) == 0 || code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}

void removeStaleFiles(const fs::path &dir)
{
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        char *rest = nullptr;
        const unsigned long pid = std::strtoul(name.c_str(), &rest, 10);
        if (rest == name.c_str() || pid == 0 || pid == GetCurrentProcessId()) {
            continue;
        }
        const std::string tail = rest;
        if ((tail == ".token" || tail == ".port" || tail == "-screenshot.png") && !processRuns(pid)) {
            std::error_code ignored;
            fs::remove(it->path(), ignored);
        }
    }
}

void onSignal(int sig)
{
    if (signalToken[0] != '\0') {
        DeleteFileA(signalToken);
    }
    if (signalPort[0] != '\0') {
        DeleteFileA(signalPort);
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void setBlocking(Socket socket, bool blocking)
{
    u_long nonBlocking = blocking ? 0 : 1;
    ioctlsocket(static_cast<SOCKET>(socket), FIONBIO, &nonBlocking);
}

} // namespace

bool openChannel(int port, Channel &c, std::string &token, std::string &listening, std::string &error)
{
    WSADATA data;
    if (!winsockStarted) {
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            error = "Winsock does not start";
            return false;
        }
        winsockStarted = true;
    }
    c.local = false; // (TCP only: port < 0 means a free port, as 0)

    const char *base = std::getenv("LOCALAPPDATA");
    if (base == nullptr || *base == '\0') {
        base = std::getenv("TEMP");
    }
    if (base == nullptr || *base == '\0') {
        error = "no %LOCALAPPDATA% for the token file";
        return false;
    }
    const fs::path dir = fs::path(base) / "one-debugger";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec)) {
        error = "cannot create " + dir.string();
        return false;
    }
    c.dir = dir.string();
    removeStaleFiles(dir);

    // The token: 32 random bytes as hex, in a file in the user's own profile
    unsigned char bytes[32];
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        error = "no random numbers";
        return false;
    }
    token.clear();
    for (unsigned char b : bytes) {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x", b);
        token += hex;
    }
    const fs::path tokenFile = dir / (std::to_string(processId()) + ".token");
    c.tokenPath = tokenFile.string();
    fs::remove(tokenFile, ec); // (a file of this pid's: a debugger before it that ended without deleting it)
    {
        std::ofstream out(tokenFile, std::ios::binary | std::ios::trunc);
        out << token;
        if (!out.flush()) {
            error = "cannot write " + c.tokenPath;
            c.tokenPath.clear();
            return false;
        }
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port < 0 ? 0 : port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOL exclusive = TRUE; // (no other program shares the port)
    int size = sizeof(address);
    if (listener != INVALID_SOCKET) {
        c.listener = static_cast<Socket>(listener);
    }
    if (listener == INVALID_SOCKET ||
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive),
                   sizeof(exclusive)) != 0 ||
        bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(listener, 2) != 0 ||
        getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size) != 0) {
        error = "cannot listen on 127.0.0.1:" + std::to_string(port < 0 ? 0 : port) + " (Winsock error " +
                std::to_string(WSAGetLastError()) + ")";
        closeChannel(c);
        return false;
    }
    setBlocking(c.listener, false);
    const std::string where = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    const fs::path portFile = dir / (std::to_string(processId()) + ".port");
    c.socketPath = portFile.string(); // (the file that says where to connect)
    std::ofstream(portFile, std::ios::binary | std::ios::trunc) << where << "\n";
    listening = "listening on " + where + ", token " + c.tokenPath;
    std::snprintf(signalToken, sizeof(signalToken), "%s", c.tokenPath.c_str());
    std::snprintf(signalPort, sizeof(signalPort), "%s", c.socketPath.c_str());
    return true;
}

void closeChannel(Channel &c)
{
    if (c.listener >= 0) {
        closesocket(static_cast<SOCKET>(c.listener));
        c.listener = kNoSocket;
    }
    for (std::string *file : {&c.tokenPath, &c.socketPath}) {
        if (!file->empty()) {
            std::error_code ec;
            fs::remove(fs::path(*file), ec);
            file->clear();
        }
    }
    signalToken[0] = signalPort[0] = '\0';
}

void handleSignals(bool always)
{
    for (int sig : {SIGINT, SIGTERM}) {
        auto previous = std::signal(sig, onSignal);
        if (!always && previous != SIG_DFL && previous != SIG_ERR) {
            std::signal(sig, previous); // (the UI's own handler stays)
        }
    }
}

bool realPath(const std::string &path, std::string &real)
{
    std::error_code ec;
    fs::path canonical = fs::canonical(fs::path(path), ec);
    if (ec) {
        return false;
    }
    real = canonical.generic_string(); // (forward slashes, as remote.cpp compares them; Windows ignores the case)
    for (char &c : real) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return true;
}

long processId()
{
    return static_cast<long>(GetCurrentProcessId());
}

Socket acceptClient(Socket listener)
{
    SOCKET client = accept(static_cast<SOCKET>(listener), nullptr, nullptr);
    return client == INVALID_SOCKET ? kNoSocket : static_cast<Socket>(client);
}

bool peerIsThisUser(Socket, const Channel &)
{
    return true; // (TCP on 127.0.0.1: the token decides)
}

void configureClient(Socket client)
{
    setBlocking(client, true); // (replies are written whole; a client that stops reading is dropped)
    DWORD limit = 5000;
    setsockopt(static_cast<SOCKET>(client), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&limit),
               sizeof(limit));
}

void closeSocket(Socket socket)
{
    closesocket(static_cast<SOCKET>(socket));
}

void sendBestEffort(Socket socket, const std::string &text)
{
    setBlocking(socket, false);
    (void)!send(static_cast<SOCKET>(socket), text.data(), static_cast<int>(text.size()), 0);
}

std::ptrdiff_t receive(Socket client, char *buffer, std::size_t size)
{
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(static_cast<SOCKET>(client), &readable);
    timeval none{0, 0};
    const int ready = select(0, &readable, nullptr, nullptr, &none);
    if (ready == 0) {
        return -1;
    }
    if (ready < 0) {
        return 0;
    }
    const int n = recv(static_cast<SOCKET>(client), buffer, static_cast<int>(size), 0);
    return n > 0 ? n : 0; // (0: closed; an error, such as a reset: the client is gone)
}

std::ptrdiff_t sendSome(Socket client, const char *data, std::size_t size)
{
    return send(static_cast<SOCKET>(client), data, static_cast<int>(size), 0);
}

void waitForInput(Socket listener, Socket client, int milliseconds)
{
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(static_cast<SOCKET>(listener), &readable);
    if (client >= 0) {
        FD_SET(static_cast<SOCKET>(client), &readable);
    }
    timeval limit{milliseconds / 1000, (milliseconds % 1000) * 1000};
    select(0, &readable, nullptr, nullptr, &limit);
}

} // namespace remote_os
// NOLINTEND(misc-include-cleaner)
