# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# one-debugger: SoftICE-style debugger for ONE, written in C++. Its user interface is Dear ImGui, in a window of its
# own next to the game window (debugger/imgui/; needs SDL2). Everything else (commands, disassembler, labels, theme...)
# is independent of it. C++ is enabled here only; the emulator core (one_core_debug) stays C and is linked as a library.
enable_language(CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT ONE_SDL2_TARGET)
    message(FATAL_ERROR "one-debugger's user interface needs SDL2: ONE_ENABLE_SDL2=ON")
endif()
include(${PROJECT_SOURCE_DIR}/cmake/imgui.cmake) # (one_imgui; gui/ may have made it already)

add_executable(one-debugger
    main.cpp
    emulator.cpp   emulator.hpp
    disasm.cpp     disasm.hpp
    cdl.cpp        cdl.hpp
    flow.cpp       flow.hpp
    symbols.cpp    symbols.hpp
    commands.cpp   commands.hpp
    remote.hpp
    oam.cpp        oam.hpp
    config.cpp     config.hpp
    theme.cpp      theme.hpp
    settings.cpp   settings.hpp
    search.cpp     search.hpp
    gamegenie.cpp  gamegenie.hpp
    watches.cpp    watches.hpp
    window_places.cpp window_places.hpp
    quickref.cpp   quickref.hpp
    viewer_window.cpp viewer_window.hpp
    bg_viewer.cpp  bg_viewer.hpp
    palette_viewer.cpp palette_viewer.hpp
    pattern_viewer.cpp pattern_viewer.hpp
    profiler.cpp profiler.hpp
    profiler_window.cpp profiler_window.hpp
    core.hpp)
# The remote channel (MCP adapter): remote.cpp is the protocol, remote_os.hpp the sockets and files it needs, which
# remote_posix.cpp (Unix socket or TCP) and remote_win.cpp (TCP, Winsock) provide
if(WIN32)
    target_sources(one-debugger PRIVATE remote.cpp remote_os.hpp remote_win.cpp)
    target_link_libraries(one-debugger PRIVATE ws2_32 bcrypt)
else()
    target_sources(one-debugger PRIVATE remote.cpp remote_os.hpp remote_posix.cpp)
endif()
target_link_libraries(one-debugger PRIVATE one_core_debug)
# Help > Quick Reference finds the PDF installed (share/doc/one), else in this source tree (quickref.cpp)
include(GNUInstallDirs)
target_compile_definitions(one-debugger PRIVATE ONE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                                                ONE_INSTALL_DOCDIR="${CMAKE_INSTALL_DOCDIR}")
target_sources(one-debugger PRIVATE
    imgui/app.cpp     imgui/app.hpp       # the window, menus, status line, keys, the emulation loop
    imgui/views.cpp   imgui/views.hpp     # the debugger's windows, drawn as a grid of characters
    imgui/dialogs.cpp imgui/dialogs.hpp)  # file dialog, message box, input box
target_link_libraries(one-debugger PRIVATE one_imgui)
target_compile_definitions(one-debugger PRIVATE ONE_DEBUGGER_UI_IMGUI)

if(MSVC)
    target_compile_options(one-debugger PRIVATE /W3 /utf-8 $<$<BOOL:${ONE_WARNINGS_AS_ERRORS}>:/WX>)
else()
    target_compile_options(one-debugger PRIVATE -Wall $<$<BOOL:${ONE_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

include(GNUInstallDirs)
install(TARGETS one-debugger RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${PROJECT_SOURCE_DIR}/docs/quickref/one-debugger-quickref.pdf DESTINATION ${CMAKE_INSTALL_DOCDIR})
