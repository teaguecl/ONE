# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Cross-compile ONE for 64-bit Windows from Linux with a MinGW-w64 toolchain
# (Debian/Ubuntu: sudo apt install mingw-w64; or llvm-mingw).
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake -DONE_FETCH_SDL2=ON
# Override the compiler prefix with -DMINGW_PREFIX=... if yours differs.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT MINGW_PREFIX)
    set(MINGW_PREFIX x86_64-w64-mingw32)
endif()
set(CMAKE_C_COMPILER   ${MINGW_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${MINGW_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${MINGW_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# A self-contained .exe: no libgcc / libwinpthread DLLs needed next to it.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
