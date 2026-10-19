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
// The remote channel's operating system layer on Linux (remote_os.hpp): a Unix socket in $XDG_RUNTIME_DIR/one-debugger
// (peer checked with SO_PEERCRED), or TCP on 127.0.0.1.
#include "remote_os.hpp"

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <arpa/inet.h> // IWYU pragma: keep
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace remote_os {

namespace {

// --- files: a private directory, stale files of ended debuggers ----------------------------------------------------

bool privateDirectory(const std::string &dir, std::string &error)
{
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        error = "cannot create " + dir + ": " + std::strerror(errno);
        return false;
    }
    struct stat st {};
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0) {
        error = dir + " must be a directory of this user's that only they can use (mode 0700)";
        return false;
    }
    return true;
}

void removeStaleFiles(const std::string &dir)
{
    DIR *d = opendir(dir.c_str());
    if (d == nullptr) {
        return;
    }
    while (const dirent *entry = readdir(d)) {
        char *end = nullptr;
        long pid = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || pid <= 0 || pid == getpid()) {
            continue;
        }
        std::string rest = end;
        if (rest != ".token" && rest != ".sock" && rest != "-screenshot.png") {
            continue;
        }
        if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
            unlink((dir + "/" + entry->d_name).c_str());
        }
    }
    closedir(d);
}

// For the signal handler: the files to delete
char signalSocket[256], signalToken[256];

void onSignal(int sig)
{
    if (signalSocket[0] != '\0') {
        unlink(signalSocket);
    }
    if (signalToken[0] != '\0') {
        unlink(signalToken);
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

} // namespace

bool openChannel(int port, Channel &c, std::string &token, std::string &listening, std::string &error)
{
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime != nullptr && runtime[0] == '/') {
        c.dir = std::string(runtime) + "/one-debugger";
    } else {
        c.dir = "/tmp/one-debugger-" + std::to_string(getuid());
    }
    if (!privateDirectory(c.dir, error)) {
        return false;
    }
    removeStaleFiles(c.dir);
    std::string base = c.dir + "/" + std::to_string(getpid());

    // The token: 32 random bytes as hex, in a file only this user can read
    unsigned char bytes[32];
    if (getrandom(bytes, sizeof(bytes), 0) != static_cast<ssize_t>(sizeof(bytes))) {
        error = std::string("no random numbers: ") + std::strerror(errno);
        return false;
    }
    token.clear();
    for (unsigned char b : bytes) {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x", b);
        token += hex;
    }
    c.tokenPath = base + ".token";
    unlink(c.tokenPath.c_str()); // (a file of this pid's: a debugger before it that ended without deleting it)
    int fd = open(c.tokenPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || write(fd, token.data(), token.size()) != static_cast<ssize_t>(token.size()) || close(fd) != 0) {
        error = "cannot write " + c.tokenPath + ": " + std::strerror(errno);
        c.tokenPath.clear();
        return false;
    }

    c.local = port < 0;
    int listenFd = -1;
    if (c.local) {
        c.socketPath = base + ".sock";
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (c.socketPath.size() >= sizeof(address.sun_path)) {
            error = "the socket's path is too long: " + c.socketPath;
            closeChannel(c);
            return false;
        }
        std::memcpy(address.sun_path, c.socketPath.c_str(), c.socketPath.size() + 1);
        unlink(c.socketPath.c_str());
        listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        c.listener = listenFd;
        if (listenFd < 0 || bind(listenFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            chmod(c.socketPath.c_str(), 0600) != 0 || listen(listenFd, 2) != 0) {
            error = "cannot listen on " + c.socketPath + ": " + std::strerror(errno);
            closeChannel(c);
            return false;
        }
        listening = "listening on " + c.socketPath;
    } else {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        listenFd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        c.listener = listenFd;
        int yes = 1;
        socklen_t size = sizeof(address);
        if (listenFd < 0 || setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0 ||
            bind(listenFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(listenFd, 2) != 0 ||
            getsockname(listenFd, reinterpret_cast<sockaddr *>(&address), &size) != 0) {
            error = "cannot listen on 127.0.0.1:" + std::to_string(port) + ": " + std::strerror(errno);
            closeChannel(c);
            return false;
        }
        listening = "listening on 127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    }
    listening += ", token " + c.tokenPath;
    std::snprintf(signalSocket, sizeof(signalSocket), "%s", c.socketPath.c_str());
    std::snprintf(signalToken, sizeof(signalToken), "%s", c.tokenPath.c_str());
    return true;
}

void closeChannel(Channel &c)
{
    if (c.listener >= 0) {
        close(static_cast<int>(c.listener));
        c.listener = kNoSocket;
    }
    if (!c.socketPath.empty()) {
        unlink(c.socketPath.c_str());
        c.socketPath.clear();
    }
    if (!c.tokenPath.empty()) {
        unlink(c.tokenPath.c_str());
        c.tokenPath.clear();
    }
    signalSocket[0] = signalToken[0] = '\0';
}

void handleSignals(bool always)
{
    for (int sig : {SIGTERM, SIGINT, SIGHUP}) {
        struct sigaction current {};
        if (sigaction(sig, nullptr, &current) == 0 && (always || current.sa_handler == SIG_DFL)) {
            std::signal(sig, onSignal);
        }
    }
}

bool realPath(const std::string &path, std::string &real)
{
    char *resolved = ::realpath(path.c_str(), nullptr);
    if (resolved == nullptr) {
        return false;
    }
    real = resolved;
    std::free(resolved);
    return true;
}

long processId()
{
    return static_cast<long>(getpid());
}

Socket acceptClient(Socket listener)
{
    return ::accept4(static_cast<int>(listener), nullptr, nullptr, SOCK_CLOEXEC);
}

bool peerIsThisUser(Socket client, const Channel &channel)
{
    if (!channel.local) {
        return true;
    }
    ucred peer{};
    socklen_t size = sizeof(peer);
    return getsockopt(static_cast<int>(client), SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 && peer.uid == getuid();
}

void configureClient(Socket client)
{
    timeval limit{5, 0}; // (replies are written whole; a client that stops reading is dropped)
    setsockopt(static_cast<int>(client), SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
}

void closeSocket(Socket socket)
{
    close(static_cast<int>(socket));
}

void sendBestEffort(Socket socket, const std::string &text)
{
    (void)!send(static_cast<int>(socket), text.data(), text.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
}

std::ptrdiff_t receive(Socket client, char *buffer, std::size_t size)
{
    while (true) {
        ssize_t n = recv(static_cast<int>(client), buffer, size, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return -1;
        }
        return n < 0 ? 0 : n;
    }
}

std::ptrdiff_t sendSome(Socket client, const char *data, std::size_t size)
{
    while (true) {
        ssize_t n = send(static_cast<int>(client), data, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return n;
    }
}

void waitForInput(Socket listener, Socket client, int milliseconds)
{
    pollfd fds[2] = {{static_cast<int>(listener), POLLIN, 0}, {static_cast<int>(client), POLLIN, 0}};
    ::poll(fds, client >= 0 ? 2 : 1, milliseconds);
}

} // namespace remote_os
