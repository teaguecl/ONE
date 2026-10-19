# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# ONE - NES emulator
#
# Quick start (see BUILDING.md for details):
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release      # or: cmake --preset linux-release
#   cmake --build build
#   ctest --test-dir build                               # smoke tests
#
cmake_minimum_required(VERSION 3.16)

project(one
    VERSION 0.1.0
    DESCRIPTION "ONE NES emulator"
    LANGUAGES C)

# ------------------------------------------------------------------------------------------------------------------
# Options
# ------------------------------------------------------------------------------------------------------------------
option(ONE_ENABLE_SDL2          "Build the SDL2 window / keyboard front end (OFF = headless-only build)" ON)
option(ONE_FETCH_SDL2           "Download and build SDL2 from source if it is not installed"            OFF)
option(ONE_MESEN_COMPATIBILITY  "Emulate Mesen-specific (undocumented) behaviour, for trace matching"    ON)
option(ONE_ENABLE_SANITIZERS    "Build with AddressSanitizer (+ UndefinedBehaviorSanitizer on GCC/Clang)" OFF)
option(ONE_WARNINGS_AS_ERRORS   "Treat compiler warnings as errors"                                       OFF)
option(ONE_FORCE_COMPAT_GETOPT  "Use the bundled getopt_long even if the platform has one (testing)"     OFF)
option(ONE_BUILD_DEBUGGER       "Build one-debugger, the SoftICE-style debugger (C++)"                     OFF)
option(ONE_PPU_CATCHUP          "FEATURE_PPU_CATCHUP in 'one' and the libretro core: the CPU runs ahead of the PPU while it cannot see it (faster, same results)" ON)
option(ONE_PPU_CATCHUP_DEBUGGER "FEATURE_PPU_CATCHUP in one-debugger too (the debugger then catches the PPU up at every instruction)" OFF)
option(ONE_BUILD_LIBRETRO       "Build one_libretro.so, ONE as a libretro core for RetroArch (libretro/)"   OFF)
option(ONE_BUILD_GUI            "The menu of the game window (C++, Dear ImGui; needs the SDL2 front end)"  ON)
set(ONE_PGO "" CACHE STRING "Profile-guided optimisation of 'one': generate | use | empty (off); tools/pgo.sh does both steps")
set_property(CACHE ONE_PGO PROPERTY STRINGS "" generate use)
set(ONE_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-profile" CACHE PATH "Where ONE_PGO=generate writes the profile and =use reads it")
option(ONE_CLANG_TIDY           "Run clang-tidy on ONE's sources while compiling them (slow; see .clang-tidy)" OFF)
option(BUILD_TESTING            "Register the CTest smoke tests"                                          ON)

if(BUILD_TESTING)
    enable_testing()   # (before any directory registers tests: gui/ does too)
endif()

# Single-config generators (Makefiles, Ninja): default to an optimised build with debug info.
get_property(ONE_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT ONE_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)                 # gnu11 on GCC/Clang: POSIX helpers (fileno, strncasecmp) stay visible
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)      # compile_commands.json for clangd / IDEs

# ------------------------------------------------------------------------------------------------------------------
# Sources
# ------------------------------------------------------------------------------------------------------------------
set(ONE_CORE_SOURCES
    src/version.c
    src/cpu.c            # also #includes src/rp2a03.c (the 2A03 APU lives on the same chip)
    src/ppu.c
    src/mapper.c
    src/mappers/mapper_nrom.c
    src/mappers/mapper_discrete.c
    src/mappers/mapper_uxrom.c
    src/mappers/mapper_cnrom.c
    src/mappers/mapper_colordreams.c
    src/mappers/mapper_bnrom.c
    src/mappers/mapper_gxrom.c
    src/mappers/mapper_camerica.c
    src/mappers/mapper_nina003.c
    src/mappers/mapper_irem74161.c
    src/mappers/mapper_bandai74161.c
    src/mappers/mapper_vrc1.c
    src/mappers/mapper_jaleco86.c
    src/mappers/mapper_jaleco87.c
    src/mappers/mapper_nina006.c
    src/mappers/mapper_sunsoft1.c
    src/mappers/mapper_cnrom185.c
    src/mappers/mapper_irem_g101.c
    src/mappers/mapper_taito_tc0190.c
    src/mappers/mapper_quattro.c
    src/mappers/mapper_sachen243.c
    src/mappers/mapper_banked.c
    src/mappers/mapper_axrom.c
    src/mappers/mapper_mmc1.c
    src/mappers/mapper_txrom.c
    src/mappers/mapper_exrom.c
    src/mappers/mapper_pxrom.c
    src/mappers/mapper_fxrom.c
    src/mappers/mapper_vrc24.c
    src/mappers/mapper_fme7.c
    src/mappers/mapper_bandai_fcg.c
    src/mappers/mapper_nes_event.c
    src/mappers/mapper_nina001.c
    src/mappers/mapper_namco163.c
    src/mappers/mapper_vrc6.c
    src/mappers/mapper_vrc6a.c
    src/mappers/mapper_vrc6b.c
    src/mappers/mapper_vrc4ac.c
    src/mappers/mapper_vrc2a.c
    src/mappers/mapper_vrc2b.c
    src/mappers/mapper_vrc4bd.c
    src/cheats.c         # Game Genie codes (src/cheats.h)
    src/crc32.c
    src/sha1.c
    src/ines.c
    src/zipfile.c        # games inside .zip files (src/zipfile.h)
    src/memory.c
    src/emu.c
    src/scheduler.c
    src/trace.c
    src/input.c
    src/zapper.c         # the Zapper light gun on port 2 (src/zapper.h)
    src/cfgfile.c        # one.cfg reader
    src/video.c
    src/audio.c          # sound output: filters, WAV file, SDL2 device
    src/png.c            # screenshots
    src/savestate.c      # save states
    src/log.c            # console messages (src/log.h)
    src/settings.c       # the settings table: one.cfg, --write-config, the menu (src/settings.h)
    src/frontend.c       # what the menu (gui/) may use of the emulator (src/frontend.h)
    src/menu_hint.c      # "Esc: menu" for a few seconds when a game starts (src/menu_hint.h)
    src/script.c         # Lua scripts, Mesen 2 style (src/script.h)
    src/script_window.c  # Lua scripts' windows of their own (one.openWindow, src/script_window.h)
    third_party/libco/libco.c    # picks the right backend (amd64.c, x86.c, fiber.c, ...) with #include
)

# Files that are #included into another .c file: listed so IDEs show them and CMake tracks them, never compiled.
set(ONE_INCLUDED_SOURCES
    src/rp2a03.c
    third_party/libco/amd64.c third_party/libco/x86.c third_party/libco/fiber.c third_party/libco/sjlj.c third_party/libco/ucontext.c
    third_party/libco/arm.c third_party/libco/aarch64.c third_party/libco/ppc.c third_party/libco/ppc64v2.c)
set_source_files_properties(${ONE_INCLUDED_SOURCES} PROPERTIES HEADER_FILE_ONLY ON)

# Headers are listed explicitly (no file(GLOB)): a glob also picks up editor lock files such as Emacs' .#src/cpu.h.
set(ONE_HEADERS
    src/audio.h src/bits.h src/cfgfile.h src/cheats.h src/crc32.h src/sha1.h src/cpu.h src/debugger_priv.h src/debug_hooks.h src/emu.h src/ines.h src/input.h src/log.h
    src/mapper.h src/mappers/mapper_discrete.h src/mappers/mapper_uxrom.h src/mappers/mapper_cnrom.h src/mappers/mapper_colordreams.h src/mappers/mapper_bnrom.h src/mappers/mapper_gxrom.h src/mappers/mapper_camerica.h src/mappers/mapper_nina003.h src/mappers/mapper_irem74161.h src/mappers/mapper_bandai74161.h src/mappers/mapper_vrc1.h src/mappers/mapper_jaleco86.h src/mappers/mapper_jaleco87.h src/mappers/mapper_nina006.h src/mappers/mapper_sunsoft1.h src/mappers/mapper_cnrom185.h src/mappers/mapper_irem_g101.h src/mappers/mapper_taito_tc0190.h src/mappers/mapper_quattro.h src/mappers/mapper_sachen243.h src/mappers/mapper_banked.h src/mappers/mapper_axrom.h src/mappers/mapper_mmc1.h src/mappers/mapper_nrom.h src/mappers/mapper_pxrom.h src/mappers/mapper_fxrom.h src/mappers/mapper_vrc24.h src/mappers/mapper_fme7.h src/mappers/mapper_bandai_fcg.h src/mappers/mapper_nes_event.h src/mappers/mapper_nina001.h src/mappers/mapper_namco163.h src/mappers/mapper_vrc6.h src/mappers/mapper_vrc6a.h src/mappers/mapper_vrc6b.h src/mappers/mapper_vrc4ac.h src/mappers/mapper_vrc2a.h src/mappers/mapper_vrc2b.h src/mappers/mapper_vrc4bd.h src/mappers/mapper_txrom.h src/mappers/mapper_exrom.h src/memory.h src/png.h src/ppu.h src/savestate.h src/font5x7.h
    src/rp2a03.h src/scheduler.h src/timing.h src/trace.h src/video.h src/version.h src/settings.h src/frontend.h src/menu_hint.h src/script.h src/script_window.h
    third_party/libco/libco.h third_party/libco/settings.h third_party/libco/valgrind.h)

# The emulator core is C only. C++ is allowed in debugger/ and gui/ only: refuse to configure if a C++ file
# shows up next to the core sources, and check that the core targets only ever contain C files.
file(GLOB ONE_STRAY_CXX ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/mappers/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libco/*.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/compat/*.cpp)
if(ONE_STRAY_CXX)
    message(FATAL_ERROR "C++ files found in the emulator core (only debugger/ and gui/ may contain C++): ${ONE_STRAY_CXX}")
endif()

# ------------------------------------------------------------------------------------------------------------------
# Lua 5.4 for scripts (script.c), downloaded and built here as a static library: always part of the build; it costs
# the emulation nothing unless a script is loaded (script.h). Lua is C, so the core stays C only.
# ------------------------------------------------------------------------------------------------------------------
include(FetchContent)
set(ONE_LUA_GIT_TAG v5.4.7 CACHE STRING "Lua release to build (github.com/lua/lua tag)")
message(STATUS "ONE: fetching Lua (${ONE_LUA_GIT_TAG})")
FetchContent_Declare(lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        ${ONE_LUA_GIT_TAG}
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(lua)   # (Lua has no CMakeLists.txt: this only downloads it; the library is defined below)
set(ONE_LUA_SOURCES lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring
                    ltable ltm lundump lvm lzio lauxlib lbaselib lcorolib ldblib liolib lmathlib loadlib loslib
                    lstrlib ltablib lutf8lib linit)
list(TRANSFORM ONE_LUA_SOURCES PREPEND ${lua_SOURCE_DIR}/)
list(TRANSFORM ONE_LUA_SOURCES APPEND .c)
add_library(one_lua STATIC ${ONE_LUA_SOURCES})
target_include_directories(one_lua SYSTEM PUBLIC ${lua_SOURCE_DIR})
if(UNIX)
    target_compile_definitions(one_lua PRIVATE LUA_USE_POSIX)   # (not LUA_USE_LINUX: no C modules, no libdl)
    target_link_libraries(one_lua PUBLIC m)
endif()
target_compile_options(one_lua PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/W0,-w>)   # third-party code
if(WIN32)
    # Lua errors are longjmps, made on libco stacks: Windows' longjmp must not unwind (see the header)
    target_compile_options(one_lua PRIVATE
        "$<IF:$<C_COMPILER_ID:MSVC>,/FI${CMAKE_CURRENT_SOURCE_DIR}/cmake/lua_setjmp_nounwind.h,SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/cmake/lua_setjmp_nounwind.h>")
endif()
set_target_properties(one_lua PROPERTIES C_CLANG_TIDY "" SKIP_LINTING ON)

# lua/fceux.lua (the FCEUX Lua API on top of the Mesen one, script.c) is compiled in as a C array
set(ONE_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(OUTPUT ${ONE_GENERATED_DIR}/fceux_lua.h
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/lua/fceux.lua
            -DOUTPUT=${ONE_GENERATED_DIR}/fceux_lua.h -DNAME=fceux_lua -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS lua/fceux.lua cmake/embed.cmake
    COMMENT "Embedding lua/fceux.lua")
# data/cheats/default.txt (the Game Genie codes built in, cheats.c; more come from the user's own file) likewise
add_custom_command(OUTPUT ${ONE_GENERATED_DIR}/gamegenie_db.h
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/data/cheats/default.txt
            -DOUTPUT=${ONE_GENERATED_DIR}/gamegenie_db.h -DNAME=gamegenie_db -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS data/cheats/default.txt cmake/embed.cmake
    COMMENT "Embedding data/cheats/default.txt")
# one_version.h (cmake/version.cmake): the version and the commit; rewritten only when its text changes
add_custom_target(one_version
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DBASE_VERSION=${PROJECT_VERSION}
            -DOUTPUT=${ONE_GENERATED_DIR}/one_version.h -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake
    BYPRODUCTS ${ONE_GENERATED_DIR}/one_version.h
    COMMENT "Writing one_version.h")
add_custom_target(one_generated DEPENDS ${ONE_GENERATED_DIR}/fceux_lua.h ${ONE_GENERATED_DIR}/gamegenie_db.h)
add_dependencies(one_generated one_version)

# Everything except main() goes into a static library. It is built twice:
#   one_core        -- for 'one': no debugger hooks at all
#   one_core_debug  -- for 'one-debugger' (ONE_BUILD_DEBUGGER=ON): ONE_DEBUGGER adds the instruction-boundary and
#                      memory-access hooks (debug_hooks.h), so the normal build pays nothing for them
function(one_add_core name)
    add_library(${name} STATIC ${ONE_CORE_SOURCES} ${ONE_INCLUDED_SOURCES} ${ONE_HEADERS} ${ARGN})
    target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party
                                           ${CMAKE_CURRENT_SOURCE_DIR})   # (the root: "gui/gui.h")
    target_include_directories(${name} PRIVATE ${ONE_GENERATED_DIR})
    if(("${name}" STREQUAL "one_core_debug" AND ONE_PPU_CATCHUP_DEBUGGER) OR (NOT "${name}" STREQUAL "one_core_debug" AND ONE_PPU_CATCHUP))
        target_compile_definitions(${name} PUBLIC FEATURE_PPU_CATCHUP)
    endif()
    add_dependencies(${name} one_generated)
    target_compile_definitions(${name} PUBLIC
        DEBUGGING_ENABLED                      # trace output (Mesen format) and the cpu_debug_* accessors
        $<$<BOOL:${ONE_MESEN_COMPATIBILITY}>:MESEN_COMPATIBILITY>
        $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
        $<$<BOOL:${MINGW}>:__USE_MINGW_ANSI_STDIO=1>)   # MinGW: printf with %zu, %llu (not the old msvcrt one)
    set_target_properties(${name} PROPERTIES LINKER_LANGUAGE C)
    target_link_libraries(${name} PUBLIC one_lua)
    if(UNIX)
        target_link_libraries(${name} PUBLIC m)   # libm (audio.c)
    endif()
    get_target_property(srcs ${name} SOURCES)
    foreach(src IN LISTS srcs)
        if(NOT src MATCHES "\\.(c|h)$")
            message(FATAL_ERROR "${name}: '${src}' is not a C file; the emulator core must stay C only")
        endif()
    endforeach()
endfunction()

one_add_core(one_core)
set(ONE_CORE_TARGETS one_core)
if(ONE_BUILD_DEBUGGER)
    one_add_core(one_core_debug src/debug_hooks.c)
    target_compile_definitions(one_core_debug PUBLIC ONE_DEBUGGER)
    list(APPEND ONE_CORE_TARGETS one_core_debug)
endif()

add_executable(one src/one.c)
target_link_libraries(one PRIVATE one_core)

# ------------------------------------------------------------------------------------------------------------------
# getopt_long: glibc / MinGW have it, MSVC does not -> bundled third_party/compat/getopt.c
# ------------------------------------------------------------------------------------------------------------------
include(CheckSymbolExists)
check_symbol_exists(getopt_long "getopt.h" ONE_HAVE_GETOPT_LONG)
if(NOT ONE_HAVE_GETOPT_LONG OR ONE_FORCE_COMPAT_GETOPT)
    message(STATUS "ONE: using the bundled getopt_long (third_party/compat/)")
    target_sources(one PRIVATE third_party/compat/getopt.c third_party/compat/getopt.h)
    target_include_directories(one BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/compat)
endif()

# ------------------------------------------------------------------------------------------------------------------
# SDL2
#   1. an installed SDL2 that ships a CMake package (Linux distros, vcpkg, MSYS2, SDL2 dev zips via SDL2_DIR)
#   2. pkg-config (older Linux distros)
#   3. ONE_FETCH_SDL2=ON: download and build SDL2 as part of this build (no install needed; used by CI on Windows)
# ------------------------------------------------------------------------------------------------------------------
set(ONE_SDL2_TARGET "")
if(ONE_ENABLE_SDL2)
    if(NOT ONE_FETCH_SDL2)
        find_package(SDL2 CONFIG QUIET)
        if(TARGET SDL2::SDL2)
            set(ONE_SDL2_TARGET SDL2::SDL2)
        elseif(TARGET SDL2::SDL2-static)
            set(ONE_SDL2_TARGET SDL2::SDL2-static)
        else()
            find_package(PkgConfig QUIET)
            if(PKG_CONFIG_FOUND)
                pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
                if(SDL2_FOUND)
                    set(ONE_SDL2_TARGET PkgConfig::SDL2)
                endif()
            endif()
        endif()
    endif()

    if(NOT ONE_SDL2_TARGET AND ONE_FETCH_SDL2)
        include(FetchContent)
        message(STATUS "ONE: fetching SDL2 source (ONE_FETCH_SDL2=ON)")
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON  CACHE BOOL "" FORCE)
        set(SDL_TEST   OFF CACHE BOOL "" FORCE)
        set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL2
            URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz
            URL_HASH SHA256=24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4)
        FetchContent_MakeAvailable(SDL2)
        set(ONE_SDL2_TARGET SDL2::SDL2-static)
    endif()

    if(NOT ONE_SDL2_TARGET)
        message(FATAL_ERROR
            "SDL2 not found. Either\n"
            "  - install it (Debian/Ubuntu/Mint: sudo apt install libsdl2-dev; MSYS2: pacman -S mingw-w64-ucrt-x86_64-SDL2;\n"
            "    vcpkg: vcpkg install sdl2 and configure with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake), or\n"
            "  - let CMake download and build it: -DONE_FETCH_SDL2=ON, or\n"
            "  - build without a window: -DONE_ENABLE_SDL2=OFF")
    endif()

    foreach(core IN LISTS ONE_CORE_TARGETS)
        target_link_libraries(${core} PUBLIC ${ONE_SDL2_TARGET})
        target_compile_definitions(${core} PUBLIC ONE_HAVE_SDL2)
    endforeach()
    message(STATUS "ONE: SDL2 front end enabled (${ONE_SDL2_TARGET})")
else()
    message(STATUS "ONE: SDL2 disabled, headless-only build")
endif()

# ------------------------------------------------------------------------------------------------------------------
# Warnings, sanitizers
# ------------------------------------------------------------------------------------------------------------------
foreach(tgt ${ONE_CORE_TARGETS} one)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W3 /utf-8 $<$<BOOL:${ONE_WARNINGS_AS_ERRORS}>:/WX>)
    else()
        target_compile_options(${tgt} PRIVATE -Wall $<$<BOOL:${ONE_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
endforeach()
# libco is third-party code: keep its warnings out of -Werror builds
set_source_files_properties(third_party/libco/libco.c PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/W0,-w>")

if(ONE_ENABLE_SANITIZERS)
    if(MSVC)
        # MSVC's ASan cannot be combined with the run-time checks (/RTC1) of the default Debug flags
        string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
        foreach(core IN LISTS ONE_CORE_TARGETS)
            target_compile_options(${core} PUBLIC /fsanitize=address)
        endforeach()
    else()
        set(ONE_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
        foreach(core IN LISTS ONE_CORE_TARGETS)
            target_compile_options(${core} PUBLIC ${ONE_SAN_FLAGS})
            target_link_options(${core} PUBLIC ${ONE_SAN_FLAGS})
        endforeach()
    endif()
endif()

# ------------------------------------------------------------------------------------------------------------------
# Profile-guided optimisation (GCC / Clang) of the emulator core and 'one': ONE_PGO=generate builds an instrumented
# 'one' that records where the time goes into ONE_PGO_DIR while it runs; ONE_PGO=use rebuilds with that profile
# (about 15-20% faster emulation). The two steps must use the same build directory (GCC finds a file's profile by its
# object path). tools/pgo.sh runs both, with a set of ROMs in between. one-debugger's core is not profiled.
# ------------------------------------------------------------------------------------------------------------------
if(ONE_PGO)
    if(MSVC)
        # MSVC's own PGO (link-time: /GENPROFILE, run, /USEPROFILE), for the libretro core only; tools/pgo.ps1.
        # The objects need /GL: configure with -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON.
        if(NOT ONE_BUILD_LIBRETRO)
            message(FATAL_ERROR "ONE_PGO with MSVC covers the libretro core only: configure with -DONE_BUILD_LIBRETRO=ON (tools/pgo.ps1)")
        endif()
        if(ONE_PGO STREQUAL "generate")
            set(ONE_PGO_MSVC_LINK /LTCG "/GENPROFILE:PGD=${ONE_PGO_DIR}/one_libretro.pgd")
        elseif(ONE_PGO STREQUAL "use")
            set(ONE_PGO_MSVC_LINK /LTCG "/USEPROFILE:PGD=${ONE_PGO_DIR}/one_libretro.pgd")
        else()
            message(FATAL_ERROR "ONE_PGO must be generate, use or empty, not '${ONE_PGO}'")
        endif()
        file(MAKE_DIRECTORY ${ONE_PGO_DIR})
    else()
        if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR "ONE_PGO needs GCC or Clang (this is ${CMAKE_C_COMPILER_ID})")
        endif()
        if(ONE_PGO STREQUAL "generate")
            if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
                set(ONE_PGO_FLAGS -fprofile-generate=${ONE_PGO_DIR} -fprofile-update=single)
            else()
                set(ONE_PGO_FLAGS -fprofile-generate=${ONE_PGO_DIR})
            endif()
            target_link_options(one PRIVATE ${ONE_PGO_FLAGS})
        elseif(ONE_PGO STREQUAL "use")
            if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
                # partial training: code the training runs did not reach is optimised as usual, not for size;
                # a profile older than the source is used where it still fits (a warning, not an error)
                set(ONE_PGO_FLAGS -fprofile-use=${ONE_PGO_DIR} -fprofile-partial-training -Wno-missing-profile
                                  -Wno-error=coverage-mismatch)
            else()
                set(ONE_PGO_FLAGS -fprofile-use=${ONE_PGO_DIR}/default.profdata -Wno-profile-instr-unprofiled
                                  -Wno-profile-instr-out-of-date)
            endif()
        else()
            message(FATAL_ERROR "ONE_PGO must be generate, use or empty, not '${ONE_PGO}'")
        endif()
        target_compile_options(one_core PRIVATE ${ONE_PGO_FLAGS})
        target_compile_options(one PRIVATE ${ONE_PGO_FLAGS})
    endif()
    message(STATUS "ONE: profile-guided optimisation: ${ONE_PGO} (${ONE_PGO_DIR})")
endif()

# ------------------------------------------------------------------------------------------------------------------
# The menu of the game window (C++ and Dear ImGui, all of it in gui/): 'one' only. It uses the emulator through
# frontend.h alone (ctest gui_separation checks).
# ------------------------------------------------------------------------------------------------------------------
if(ONE_BUILD_GUI AND ONE_SDL2_TARGET)
    enable_language(CXX)   # (here too: 'one' is linked as C++)
    add_subdirectory(gui)
    target_link_libraries(one PRIVATE one_gui)
    target_compile_definitions(one PRIVATE ONE_HAVE_GUI)
    set_target_properties(one PROPERTIES LINKER_LANGUAGE CXX)   # (main() is C, but the menu needs the C++ library)
    message(STATUS "ONE: menu (gui/, Dear ImGui) enabled")
endif()

# ------------------------------------------------------------------------------------------------------------------
# libretro core (libretro/libretro.c): the core again without SDL2 (the frontend
# has the window, sound and input), position independent, in a shared library that exports retro_* only.
# ------------------------------------------------------------------------------------------------------------------
if(ONE_BUILD_LIBRETRO)
    one_add_core(one_core_libretro)
    target_compile_definitions(one_core_libretro PUBLIC ONE_LIBRETRO)
    set_target_properties(one_core_libretro one_lua PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
    if(MSVC)
        target_compile_options(one_core_libretro PRIVATE /W3 /utf-8 $<$<BOOL:${ONE_WARNINGS_AS_ERRORS}>:/WX>)
    else()
        target_compile_options(one_core_libretro PRIVATE -Wall $<$<BOOL:${ONE_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
    add_library(one_libretro MODULE libretro/libretro.c libretro/libretro.h)
    set_target_properties(one_libretro PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(one_libretro PRIVATE one_core_libretro)
    if(MSVC)
        target_compile_options(one_libretro PRIVATE /W3 /utf-8 $<$<BOOL:${ONE_WARNINGS_AS_ERRORS}>:/WX>)
    else()
        target_compile_options(one_libretro PRIVATE -Wall $<$<BOOL:${ONE_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
    if(MINGW)
        target_link_options(one_libretro PRIVATE -static-libgcc -static)   # one_libretro.dll needs no other DLLs
    endif()
    if(ONE_PGO)   # (the flags are made with 'one' above; tools/pgo.sh --libretro / tools/pgo.ps1 train with tests/libretro_host.c)
        if(MSVC)
            target_link_options(one_libretro PRIVATE ${ONE_PGO_MSVC_LINK})
        else()
            target_compile_options(one_core_libretro PRIVATE ${ONE_PGO_FLAGS})
            target_compile_options(one_libretro PRIVATE ${ONE_PGO_FLAGS})
            if(ONE_PGO STREQUAL "generate")
                target_link_options(one_libretro PRIVATE ${ONE_PGO_FLAGS})
            endif()
        endif()
    endif()
    message(STATUS "ONE: libretro core enabled (one_libretro.so)")
endif()

# ------------------------------------------------------------------------------------------------------------------
# Debugger (C++, all of it in debugger/); its user interface is Dear ImGui (debugger/imgui/)
# ------------------------------------------------------------------------------------------------------------------
if(ONE_BUILD_DEBUGGER)
    add_subdirectory(debugger)
endif()

# ------------------------------------------------------------------------------------------------------------------
# Documentation: the one-debugger quick reference PDF (docs/quickref; needs pandoc and Chrome/Chromium).
# Not part of 'all': cmake --build <dir> --target quickref
# ------------------------------------------------------------------------------------------------------------------
add_custom_target(quickref
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/docs/quickref/build.sh ${CMAKE_CURRENT_BINARY_DIR}/one-debugger-quickref.pdf
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building the one-debugger quick reference (docs/quickref)"
    VERBATIM)

# ------------------------------------------------------------------------------------------------------------------
# Code formatting with clang-format 18 (style: .clang-format; script: tools/format.sh). Not part of 'all':
#   cmake --build <dir> --target format         reformat every source file
#   cmake --build <dir> --target format-check   list the files that are not formatted (fails if any)
# The ctest test clang_format runs the check too, and tools/pre-commit can run it before each git commit.
# ------------------------------------------------------------------------------------------------------------------
find_program(ONE_BASH bash)
if(ONE_BASH)
    add_custom_target(format
        COMMAND ${ONE_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tools/format.sh
        COMMENT "clang-format: reformatting the sources"
        VERBATIM)
    add_custom_target(format-check
        COMMAND ${ONE_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tools/format.sh --check
        COMMENT "clang-format: checking the formatting"
        VERBATIM)
endif()

# ------------------------------------------------------------------------------------------------------------------
# Static analysis with clang-tidy 18 (checks: .clang-tidy, debugger/.clang-tidy; script: tools/tidy.sh).
#   cmake --build <dir> --target tidy    check every source file (about a minute; not part of 'all')
#   -DONE_CLANG_TIDY=ON                  check each file as it is compiled (findings fail the build)
# The ctest test clang_tidy (label lint) runs the same check; tools/pre-commit checks the files a commit changes.
# ------------------------------------------------------------------------------------------------------------------
if(ONE_BASH)
    add_custom_target(tidy
        COMMAND ${ONE_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tools/tidy.sh -p ${CMAKE_BINARY_DIR}
        COMMENT "clang-tidy: checking the sources"
        VERBATIM)
endif()

# ------------------------------------------------------------------------------------------------------------------
# Install / package (cpack produces a .zip on Windows, .tar.gz on Linux)
# ------------------------------------------------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS one RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# Example Lua scripts (lua/examples: background.lua...) next to the other documentation
install(DIRECTORY lua/examples/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples FILES_MATCHING PATTERN "*.lua" PATTERN "*.md")
set(CPACK_PACKAGE_NAME one)
if(WIN32)
    set(CPACK_GENERATOR ZIP)
else()
    set(CPACK_GENERATOR TGZ)
endif()
include(CPack)

# ------------------------------------------------------------------------------------------------------------------
# Tests (ctest). Quick smoke tests that only need files in the repository; the Mesen trace regression
# (regression/regtest.sh, large logs) is registered when bash is available and labelled so it can be skipped.
# ------------------------------------------------------------------------------------------------------------------
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# ONE_CLANG_TIDY: clang-tidy on ONE's own targets only (not SDL, which is built in this tree too)
if(ONE_CLANG_TIDY)
    find_program(ONE_CLANG_TIDY_EXE NAMES clang-tidy-18 clang-tidy REQUIRED)
    string(REGEX REPLACE "([][.*^$+?(){}|\\])" "\\\\\\1" one_root_regex "${CMAKE_CURRENT_SOURCE_DIR}")
    set(one_tidy ${ONE_CLANG_TIDY_EXE} "--header-filter=^${one_root_regex}/(src/|src/mappers/|debugger/|debugger/imgui/|gui/)?[^/]+\\.(h|hpp)$")
    foreach(tgt one_core one_core_debug one one-debugger one_gui gui_test trace_check)
        if(TARGET ${tgt})
            set_target_properties(${tgt} PROPERTIES C_CLANG_TIDY "${one_tidy}" CXX_CLANG_TIDY "${one_tidy}")
        endif()
    endforeach()
    if(CMAKE_VERSION VERSION_LESS 3.27)
        message(FATAL_ERROR "ONE_CLANG_TIDY needs CMake 3.27 or newer (to leave the third-party libco out); "
                            "use tools/tidy.sh or the tidy target instead")
    endif()
    set_source_files_properties(third_party/libco/libco.c PROPERTIES SKIP_LINTING ON)   # third party
endif()
