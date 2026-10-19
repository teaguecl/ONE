# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Dear ImGui (https://github.com/ocornut/imgui, MIT licence): downloaded and built as part of this build, as the
# library one_imgui. It has no CMake project of its own, so its core and the two SDL2 backends (platform:
# imgui_impl_sdl2, renderer: imgui_impl_sdlrenderer2, which needs SDL 2.0.18 or newer) are compiled here. Used by the
# game window's menu (gui/) and by one-debugger's ImGui user interface (debugger/imgui/); built once for both.
include_guard(GLOBAL)

set(ONE_IMGUI_GIT_TAG v1.92.9b CACHE STRING "Dear ImGui release to build")
include(FetchContent)
message(STATUS "ONE: fetching Dear ImGui (${ONE_IMGUI_GIT_TAG})")
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        ${ONE_IMGUI_GIT_TAG}
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

add_library(one_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)       # InputText() with std::string
target_include_directories(one_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
                                                  ${imgui_SOURCE_DIR}/misc/cpp)
target_compile_definitions(one_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(one_imgui PUBLIC ${ONE_SDL2_TARGET})
# third party: no warnings from it in -Werror builds
target_compile_options(one_imgui PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>)
