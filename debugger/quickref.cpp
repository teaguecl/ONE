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
#include "quickref.hpp"
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/types.h>
#include <system_error>
#include <vector>
#ifdef _WIN32
#include <cstdint>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {
const char kFileName[] = "one-debugger-quickref.pdf";

#ifndef _WIN32
// Where PROGRAM is on the PATH; "" if it is not
std::string onPath(const char *program)
{
    const char *path = std::getenv("PATH");
    std::istringstream dirs(path != nullptr ? path : "");
    std::error_code ec;
    for (std::string dir; std::getline(dirs, dir, ':');) {
        if (!dir.empty() && fs::is_regular_file(fs::path(dir) / program, ec)) {
            return (fs::path(dir) / program).string();
        }
    }
    return {};
}
#endif
} // namespace

std::string quickReferencePath()
{
    std::error_code ec;
    std::vector<fs::path> candidates;
#ifdef _WIN32
    wchar_t exe[MAX_PATH]; // NOLINT(misc-include-cleaner): <windows.h> provides these
    DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH); // NOLINT(misc-include-cleaner)
    if (length > 0 && length < MAX_PATH) {
        candidates.push_back(fs::path(exe).parent_path() / kFileName); // (next to the .exe, as in the release zip)
    }
#else
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        candidates.push_back(self.parent_path().parent_path() / ONE_INSTALL_DOCDIR / kFileName); // (installed)
        candidates.push_back(self.parent_path().parent_path() / kFileName); // (the build directory: --target quickref)
    }
#endif
    candidates.push_back(fs::path(ONE_SOURCE_DIR) / "docs" / "quickref" / kFileName);
    for (const fs::path &candidate : candidates) {
        if (fs::is_regular_file(candidate, ec)) {
            return candidate.lexically_normal().string();
        }
    }
    return {};
}

std::string openQuickReference()
{
    const std::string path = quickReferencePath();
    if (path.empty()) {
        return std::string("Quick reference not found: ") + kFileName +
               " (cmake --build <dir> --target quickref makes it)";
    }
#ifdef _WIN32
    const auto result = reinterpret_cast<std::intptr_t>(
        ShellExecuteW(nullptr, L"open", fs::path(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        return "Quick reference: " + path + " (no program to open it)";
    }
    return "Quick reference: " + path;
#else
    const std::string opener = onPath("xdg-open"); // (found now: after fork() the child only calls exec)
    if (opener.empty()) {
        return "Quick reference: " + path + " (xdg-open is not installed to open it)";
    }
    // xdg-open in a grandchild: nothing to wait for, and no zombie
    pid_t child = fork();
    if (child == 0) {
        setsid();
        if (fork() == 0) {
            int nowhere = open("/dev/null", O_RDWR);
            if (nowhere >= 0) {
                dup2(nowhere, STDIN_FILENO);
                dup2(nowhere, STDOUT_FILENO);
                dup2(nowhere, STDERR_FILENO);
            }
            execl(opener.c_str(), "xdg-open", path.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        _exit(0);
    }
    if (child < 0) {
        return "Cannot open the quick reference: " + path;
    }
    waitpid(child, nullptr, 0);
    return "Quick reference: " + path + " (opened with xdg-open)";
#endif
}
