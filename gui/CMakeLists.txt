# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# one_gui: the menu of the game window (C++17, Dear ImGui), linked into 'one' only (not into one-debugger).
# It sees the emulator only through frontend.h (and settings.h): tools/separation_check.py (ctest gui_separation)
# checks that no other header of the core is included here, and that the core includes nothing from here except
# gui/gui.h in one.c.
enable_language(CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dear ImGui: the library one_imgui (shared with one-debugger's ImGui user interface)
include(${PROJECT_SOURCE_DIR}/cmake/imgui.cmake)

# ------------------------------------------------------------------------------------------------------------------
# The menu
# ------------------------------------------------------------------------------------------------------------------
add_library(one_gui STATIC
    gui.h            # the C interface one.c uses: gui_install()
    menu.hpp         # what the menu's files share
    menu.cpp         # the menu: window, loop, the Game page
    settings_page.cpp # the settings pages, built from the settings table (settings.h)
    gamegenie_page.cpp # the Game Genie page (frontend.h fe_gg_*)
    browser.cpp)     # choosing a ROM file
target_link_libraries(one_gui PUBLIC one_imgui PRIVATE one_core)
if(MSVC)
    target_compile_options(one_gui PRIVATE /W3 /utf-8 $<$<BOOL:${ONE_WARNINGS_AS_ERRORS}>:/WX>)
else()
    target_compile_options(one_gui PRIVATE -Wall -Wextra $<$<BOOL:${ONE_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

# A test: the menu on SDL's dummy video driver, driven by injected key presses (see gui_test.cpp)
if(BUILD_TESTING)
    add_executable(gui_test gui_test.cpp)
    target_link_libraries(gui_test PRIVATE one_gui one_core)
    add_test(NAME gui_menu COMMAND gui_test ${PROJECT_SOURCE_DIR}/tests/data/nestest.nes
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(gui_menu PROPERTIES LABELS smoke TIMEOUT 120
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/gui_test_config"
        PASS_REGULAR_EXPRESSION "gui_test: [0-9]+ checks, all passed")
endif()
