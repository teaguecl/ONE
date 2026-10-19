# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Smoke tests: ctest --test-dir build [-L smoke | -L regression]
# Labels slow (more than a few seconds) and nightly (only in the nightly tier) are used by tools/check.sh: fast = -LE
# 'slow|nightly'. Give a new test that takes more than ~10 s the label slow.

add_executable(trace_check trace_check.c)

set(ONE_ROOT ${PROJECT_SOURCE_DIR})
find_package(Python3 COMPONENTS Interpreter QUIET) # for the audio and screenshot checks

# The bundled getopt_long (third_party/compat, used when the platform has none, as with MSVC) gives the same results
# as GNU getopt for the cases in getopt_test.c; getopt_system runs the same cases on the platform's own
add_executable(getopt_test_compat getopt_test.c ${ONE_ROOT}/third_party/compat/getopt.c)
target_compile_definitions(getopt_test_compat PRIVATE GETOPT_TEST_COMPAT)
target_include_directories(getopt_test_compat BEFORE PRIVATE ${ONE_ROOT}/third_party/compat)
add_test(NAME getopt_compat COMMAND getopt_test_compat)
set_tests_properties(getopt_compat PROPERTIES LABELS smoke PASS_REGULAR_EXPRESSION "getopt_test: ok")
if(ONE_HAVE_GETOPT_LONG)
    add_executable(getopt_test_system getopt_test.c)
    add_test(NAME getopt_system COMMAND getopt_test_system)
    set_tests_properties(getopt_system PROPERTIES LABELS smoke PASS_REGULAR_EXPRESSION "getopt_test: ok")
endif()

# The emulator starts and prints its help
add_test(NAME one_help COMMAND one --help)
set_tests_properties(one_help PROPERTIES LABELS smoke PASS_REGULAR_EXPRESSION "Usage: one")

# The Zapper test ROM (tests/zapper) runs to its end with a scripted Zapper aim ("CYCLE zapper X Y [fire]" in --input
# files): 8 frames measured, done flag $A5 at $0388; the bytes used per frame: light in frames 1, 3, 4, 5 (3 changes = 9
# bytes: dark, light, dark), none in 0, 2, 6, 7. The light model itself is checked against Mesen by zapper_mesen
add_test(NAME zapper_rom COMMAND one -H --input ${ONE_ROOT}/tests/zapper/zapper_input.txt -k 400000 -D 0380-0388
    ${ONE_ROOT}/tests/zapper/zapper.nes)
set_tests_properties(zapper_rom PROPERTIES LABELS smoke TIMEOUT 60
    PASS_REGULAR_EXPRESSION "RAM 0380: 03 09 03 09 09 09 03 03 A5" FAIL_REGULAR_EXPRESSION "expected 'CYCLE")
# The Zapper palette sweep ROM (tests/zapper/sweep.nes, make_sweep.py + sweep.s): ONE's own run with an aim visiting every
# color; the analysis (sweep_analyze.py, which also reads Mesen's trace of the same ROM) must see all 64 colors and a
# consistent brightness bracket
if(Python3_FOUND)
    add_test(NAME zapper_sweep COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/zapper/sweep_analyze.py --selftest $<TARGET_FILE:one>)
    set_tests_properties(zapper_sweep PROPERTIES LABELS smoke TIMEOUT 120 PASS_REGULAR_EXPRESSION "zapper_sweep: ok")
endif()
# The Zapper against Mesen: 167,509 reads of $4017 with the aim Mesen saw (tests/zapper_check.py)
if(Python3_FOUND)
    add_test(NAME zapper_mesen COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/zapper_check.py $<TARGET_FILE:one>)
    set_tests_properties(zapper_mesen PROPERTIES LABELS smoke TIMEOUT 120
        PASS_REGULAR_EXPRESSION "zapper_check: [1-9][0-9]* reads of .4017, 0 different")
endif()

# Byte-for-byte Mesen trace comparisons that only need small files from the repository
function(one_trace_test name rom log args)
    if(EXISTS ${ONE_ROOT}/${rom} AND EXISTS ${ONE_ROOT}/${log})
        add_test(NAME ${name}
            COMMAND ${CMAKE_COMMAND}
                -DONE=$<TARGET_FILE:one>
                -DTRACE_CHECK=$<TARGET_FILE:trace_check>
                -DROM=${ONE_ROOT}/${rom}
                -DLOG=${ONE_ROOT}/${log}
                -DARGS=${args}
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/${name}.out
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_trace_test.cmake)
        set_tests_properties(${name} PROPERTIES LABELS smoke TIMEOUT 300)
    else()
        message(STATUS "ONE: skipping test ${name} (${rom} or ${log} not in the source tree)")
    endif()
endfunction()

one_trace_test(nestest tests/data/nestest.nes tests/data/nestest_mesen.txt "-p C000 -R 74,00,F4,FA,07")

# Full Mesen trace regression (regression/regtest.sh): needs bash and the (large) Mesen logs.
# Tests whose ROM or log is missing are skipped by the script.
find_program(ONE_BASH bash)

# Formatting (tools/format.sh --check): fast, so it runs with the smoke tests. Skipped (not failed) when
# clang-format 18 is missing; then install it (sudo apt install clang-format-18) or see BUILDING.md.
if(ONE_BASH)
    add_test(NAME clang_format COMMAND ${ONE_BASH} ${ONE_ROOT}/tools/format.sh --check)
    set_tests_properties(clang_format PROPERTIES LABELS "smoke;lint" TIMEOUT 120 SKIP_RETURN_CODE 2)

    # Static analysis (tools/tidy.sh): about a minute, so only with the lint label (ctest -L lint), not smoke.
    # Skipped when clang-tidy 18 is missing.
    add_test(NAME clang_tidy COMMAND ${ONE_BASH} ${ONE_ROOT}/tools/tidy.sh -p ${CMAKE_BINARY_DIR})
    set_tests_properties(clang_tidy PROPERTIES LABELS "lint;slow" TIMEOUT 900 SKIP_RETURN_CODE 2)
endif()
if(ONE_BASH)
    add_test(NAME regression_quick
        COMMAND ${ONE_BASH} ${ONE_ROOT}/regression/regtest.sh quick
        WORKING_DIRECTORY ${ONE_ROOT})
    set_tests_properties(regression_quick PROPERTIES
        LABELS "regression;slow" PROCESSORS 8
        ENVIRONMENT "ONE=$<TARGET_FILE:one>"
        FAIL_REGULAR_EXPRESSION "UNEXPECTED"
        TIMEOUT 3600)
    # The same with PPU catch-up on and a sampled trace (--trace-sample): its lines must be a subset of the Mesen log.
    add_test(NAME regression_sampled
        COMMAND ${ONE_BASH} ${ONE_ROOT}/regression/regtest.sh quick
        WORKING_DIRECTORY ${ONE_ROOT})
    set_tests_properties(regression_sampled PROPERTIES
        LABELS "regression;catchup;slow" PROCESSORS 4
        ENVIRONMENT "ONE=$<TARGET_FILE:one>;SAMPLE=30;JOBS=4"
        FAIL_REGULAR_EXPRESSION "UNEXPECTED"
        TIMEOUT 3600)
endif()

# one-debugger: scripted commands in --batch mode (no terminal needed)
if(TARGET one-debugger AND EXISTS ${ONE_ROOT}/tests/data/nestest.nes)
    add_test(NAME debugger_batch
        COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_batch.log
                -s ${CMAKE_CURRENT_SOURCE_DIR}/debugger_smoke.txt ${ONE_ROOT}/tests/data/nestest.nes)
    set_tests_properties(debugger_batch PROPERTIES LABELS smoke TIMEOUT 120 FAIL_REGULAR_EXPRESSION "ASSERTION FAILED")
    add_test(NAME debugger_zip
        COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_zip.log
                -s ${CMAKE_CURRENT_SOURCE_DIR}/debugger_zip.txt ${ONE_ROOT}/tests/data/nestest.nes
        WORKING_DIRECTORY ${ONE_ROOT})
    set_tests_properties(debugger_zip PROPERTIES LABELS smoke TIMEOUT 120 FAIL_REGULAR_EXPRESSION "ASSERTION FAILED")
    # fs / press / savestate, on a copy of nestest (the save state goes next to the ROM)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/frames_dir)
    configure_file(${ONE_ROOT}/tests/data/nestest.nes ${CMAKE_CURRENT_BINARY_DIR}/frames_dir/nestest.nes COPYONLY)
    add_test(NAME debugger_frames
        COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_frames.log
                -s ${CMAKE_CURRENT_SOURCE_DIR}/debugger_frames.txt ${CMAKE_CURRENT_BINARY_DIR}/frames_dir/nestest.nes)
    set_tests_properties(debugger_frames PROPERTIES LABELS smoke TIMEOUT 60 FAIL_REGULAR_EXPRESSION "ASSERTION FAILED")
    # The call stack (bt) and the code/data log (cdl): the log keeps each PRG bank apart, saves and loads Mesen's format
    add_test(NAME debugger_bt
        COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_bt.log
                -s ${CMAKE_CURRENT_SOURCE_DIR}/debugger_bt.txt ${ONE_ROOT}/tests/data/nestest.nes)
    set_tests_properties(debugger_bt PROPERTIES LABELS smoke TIMEOUT 60 FAIL_REGULAR_EXPRESSION "ASSERTION FAILED")
    # The profiler (prof): exact calls and cycles per routine on a small hand-assembled program (tests/profiler)
    add_test(NAME debugger_profiler
        COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_profiler.log
                -s ${CMAKE_CURRENT_SOURCE_DIR}/debugger_profiler.txt ${ONE_ROOT}/tests/profiler/profiler.nes
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(debugger_profiler PROPERTIES LABELS smoke TIMEOUT 60 FAIL_REGULAR_EXPRESSION "ASSERTION FAILED")
    # The lag frame counter (lag): frames in which the game did not read the controllers (tests/lag, tests/profiler)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lag_dir)
    configure_file(${ONE_ROOT}/tests/lag/lag.nes ${CMAKE_CURRENT_BINARY_DIR}/lag_dir/lag.nes COPYONLY)
    add_test(NAME debugger_zapper
        COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_zapper.log
                -s ${CMAKE_CURRENT_SOURCE_DIR}/debugger_zapper.txt ${ONE_ROOT}/tests/zapper/zapper_free.nes
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(debugger_zapper PROPERTIES LABELS smoke TIMEOUT 60 FAIL_REGULAR_EXPRESSION "ASSERTION FAILED"
        PASS_REGULAR_EXPRESSION "Zapper: pointing at 136,104.*Zapper unplugged.*Syntax: zapper")
    add_test(NAME debugger_lag
        COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_lag.log
                -s ${CMAKE_CURRENT_SOURCE_DIR}/debugger_lag.txt ${CMAKE_CURRENT_BINARY_DIR}/lag_dir/lag.nes
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(debugger_lag PROPERTIES LABELS smoke TIMEOUT 60 FAIL_REGULAR_EXPRESSION "ASSERTION FAILED")
    add_test(NAME debugger_lag_never
        COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_lag_never.log
                -s ${CMAKE_CURRENT_SOURCE_DIR}/debugger_lag_never.txt ${ONE_ROOT}/tests/profiler/profiler.nes
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(debugger_lag_never PROPERTIES LABELS smoke TIMEOUT 60 FAIL_REGULAR_EXPRESSION "ASSERTION FAILED")
    if(EXISTS ${ONE_ROOT}/test_roms/apu_test/rom_singles/7-dmc_basics.nes)   # (third-party ROM: tools/fetch_test_roms.sh)
        add_test(NAME debugger_cdl_pcm
            COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_cdl_pcm.log
                    -s ${CMAKE_CURRENT_SOURCE_DIR}/debugger_cdl_pcm.txt ${ONE_ROOT}/test_roms/apu_test/rom_singles/7-dmc_basics.nes)
        set_tests_properties(debugger_cdl_pcm PROPERTIES LABELS smoke TIMEOUT 60 FAIL_REGULAR_EXPRESSION "ASSERTION FAILED")
    endif()
    if(EXISTS ${ONE_ROOT}/tests/health/health.nes)
        configure_file(${ONE_ROOT}/tests/health/health.nes ${CMAKE_CURRENT_BINARY_DIR}/cdl_dir/health.nes COPYONLY)
        add_test(NAME debugger_cdl
            COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_cdl.log
                    -s ${CMAKE_CURRENT_SOURCE_DIR}/debugger_cdl.txt ${CMAKE_CURRENT_BINARY_DIR}/cdl_dir/health.nes)
        set_tests_properties(debugger_cdl PROPERTIES LABELS smoke TIMEOUT 60 FAIL_REGULAR_EXPRESSION "ASSERTION FAILED")
    endif()
    # The PRG bank in a breakpoint stop: MMC1 (tests/mmc1), a read of the switchable bank from the fixed one (bank 7)
    if(EXISTS ${ONE_ROOT}/tests/mmc1/mmc1.nes)
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/debugger_bank.txt "bpm 8000 r\nx\n")
        add_test(NAME debugger_bank
            COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_bank.log
                    -s ${CMAKE_CURRENT_BINARY_DIR}/debugger_bank.txt ${ONE_ROOT}/tests/mmc1/mmc1.nes)
        set_tests_properties(debugger_bank PROPERTIES LABELS smoke TIMEOUT 60
            PASS_REGULAR_EXPRESSION "Break due to BPM #0 \\(\\$8000 read\\) at 07:[C-F][0-9A-F][0-9A-F][0-9A-F] \\(PRG bank 7 of 8, ROM offset \\$1[C-F][0-9A-F][0-9A-F][0-9A-F]\\)")
    endif()
    # Game Genie codes (cheats.c): applied, 8-letter compare values, on/off, kept over a power cycle
    add_test(NAME debugger_gamegenie
        COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_gamegenie.log
                -s ${CMAKE_CURRENT_SOURCE_DIR}/gamegenie.txt ${ONE_ROOT}/tests/data/nestest.nes)
    set_tests_properties(debugger_gamegenie PROPERTIES LABELS smoke TIMEOUT 60 FAIL_REGULAR_EXPRESSION "ASSERTION FAILED")
    # Started without a ROM: commands that need a game say so; load brings one (another game's breakpoints and labels
    # go), a file that is not a .nes keeps the current game
    if(EXISTS ${ONE_ROOT}/tests/mmc1/mmc1.nes)
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/debugger_load.txt
            "r\n"
            "load ${ONE_ROOT}/tests/mmc1/mmc1.nes\n"
            "bpx 8000\n"
            "n 8000 mylabel\n"
            "load ${ONE_ROOT}/tests/data/nestest.nes\n"
            "bl\n"
            "u mylabel\n"
            "r pc = c000\n"
            "t 3\n"
            "!assert reg pc c5f9\n"
            "load ${ONE_ROOT}/BUILDING.md\n"
            "!assert reg pc c5f9\n")
        add_test(NAME debugger_load
            COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_load.log
                    -s ${CMAKE_CURRENT_BINARY_DIR}/debugger_load.txt)
        set_tests_properties(debugger_load PROPERTIES LABELS smoke TIMEOUT 60
            PASS_REGULAR_EXPRESSION "No game loaded[^#]*Loaded [^#]*mmc1.nes[^#]*Loaded [^#]*nestest.nes[^#]*No breakpoints set[^#]*Unknown symbol or address 'mylabel'[^#]*not an iNES file"
            FAIL_REGULAR_EXPRESSION "ASSERTION FAILED")
    endif()
    # The ImGui user interface on SDL's dummy video driver, with a game window: the -s
    # commands run in it (the windows they open, the code and memory they show), q quits it; the exit code is 1 if
    # an !assert failed. The ROM is a copy (the UI keeps a game's watch list next to it), the recent games and
    # settings are kept in the test's directory.
    set(ui_dir ${CMAKE_CURRENT_BINARY_DIR}/debugger_imgui)
    file(MAKE_DIRECTORY ${ui_dir})
    file(COPY ${ONE_ROOT}/tests/data/nestest.nes DESTINATION ${ui_dir})
    file(WRITE ${ui_dir}/script.txt
        "t 3\n"
        "!assert reg pc c008\n"
        "u c000\n"
        "d 10\n"
        "dp 3f00\n"
        "woam\n"
        "ww\n"
        "ec\n"
        "wbg\n"
        "wpal\n"
        "wpat\n"
        "wprof\n"
        "bpx c013\n"
        "g\n"
        "!assert reg pc c013\n"
        "wprof\n"
        "woam\n"
        "ec\n"
        "q\n")
    add_test(NAME debugger_imgui
        COMMAND one-debugger --no-init --log ${ui_dir}/debugger_imgui.log -s ${ui_dir}/script.txt
                ${ui_dir}/nestest.nes
        WORKING_DIRECTORY ${ui_dir})
    set_tests_properties(debugger_imgui PROPERTIES LABELS smoke TIMEOUT 60
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;XDG_CONFIG_HOME=${ui_dir}/config;XDG_STATE_HOME=${ui_dir}/state")
endif()

# The documented config files (debugger/one-debugger.cfg and one-debugger-borland.cfg, made with --write-config
# [--theme borland]) must load without complaints, i.e. they list only settings the debugger knows, with valid values.
if(TARGET one-debugger AND EXISTS ${ONE_ROOT}/tests/data/nestest.nes)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/quit.txt "q\n")
    foreach(cfg one-debugger one-debugger-borland)
        add_test(NAME debugger_config_${cfg}
            COMMAND one-debugger --batch --config ${ONE_ROOT}/debugger/${cfg}.cfg
                    --log ${CMAKE_CURRENT_BINARY_DIR}/${cfg}.log
                    -s ${CMAKE_CURRENT_BINARY_DIR}/quit.txt ${ONE_ROOT}/tests/data/nestest.nes)
        set_tests_properties(debugger_config_${cfg} PROPERTIES LABELS smoke TIMEOUT 60
            PASS_REGULAR_EXPRESSION "Settings from" FAIL_REGULAR_EXPRESSION "\\.cfg:[0-9]+:")
    endforeach()
endif()

# The one-latch mappers (mapper_discrete.c): UxROM, AxROM, Color Dreams, BNROM, GxROM, Camerica, NINA-003, Irem 74161, and the mappers 87, 86, 70, 184, 185, 113, 243, 232, 33, 75, 32, 88. tests/discrete/gen.py makes the ROMs and
# scripts: the CPU writes the bank register (with a bus conflict, and a bank number past the ROM's size) and the script
# asserts what the CPU and the PPU see (PRG banks, CHR-ROM / CHR-RAM, one-screen mirroring)
if(TARGET one-debugger)
    foreach(board uxrom axrom colordreams bnrom gxrom camerica nina003 irem74161 jaleco87 jaleco86 bandai74161 sunsoft1
                    cnrom185 cnrom185b nina006 sachen243 quattro tc0190 vrc1 irem_g101 namco88)
        add_test(NAME debugger_mapper_${board}
            COMMAND one-debugger --batch -s ${CMAKE_CURRENT_SOURCE_DIR}/discrete/${board}.txt
                    ${CMAKE_CURRENT_SOURCE_DIR}/discrete/${board}.nes)
        set_tests_properties(debugger_mapper_${board} PROPERTIES LABELS smoke TIMEOUT 60
            FAIL_REGULAR_EXPRESSION "ASSERTION FAILED|Syntax|Unknown")
    endforeach()
endif()

# The MMC5 (mapper_exrom.c). tests/exrom/gen.py makes the ROM and the script: PRG modes and RAM, CHR modes and sets, name table
# sources (CIRAM, ExRAM, fill mode), ExRAM, the multiplier, and a scanline IRQ (its line and count)
if(TARGET one-debugger)
    add_test(NAME debugger_mapper_exrom
        COMMAND one-debugger --batch -s ${CMAKE_CURRENT_SOURCE_DIR}/exrom/exrom.txt
                ${CMAKE_CURRENT_SOURCE_DIR}/exrom/exrom.nes)
    set_tests_properties(debugger_mapper_exrom PROPERTIES LABELS smoke TIMEOUT 60
        FAIL_REGULAR_EXPRESSION "ASSERTION FAILED|Syntax|Unknown")
endif()

# The MMC5's pictures (tests/exrom/pic_*.nes: extended attributes, the vertical split on the left and on the right): a screenshot of
# each, checked pixel by pixel (tests/exrom/pic_check.py)
if(TARGET one-debugger AND Python3_FOUND)
    set(exrom_pics exattr split_left split_right split_scroll_a split_scroll_b exattr_scroll chr1m chr1m_exattr split_8x16
        split_mode1 split_wrap chr1m_mode0 chr1m_mode1 chr1m_mode2)
    set(exrom_pic_tests "")
    foreach(kind ${exrom_pics})
        add_test(NAME debugger_mapper_exrom_${kind}
            COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_BINARY_DIR}
                    $<TARGET_FILE:one-debugger> --batch --log ${CMAKE_CURRENT_BINARY_DIR}/exrom_${kind}.log
                    -s ${CMAKE_CURRENT_SOURCE_DIR}/exrom/pic_${kind}.txt ${CMAKE_CURRENT_SOURCE_DIR}/exrom/pic_${kind}.nes)
        set_tests_properties(debugger_mapper_exrom_${kind} PROPERTIES LABELS smoke TIMEOUT 60
            PASS_REGULAR_EXPRESSION "Screenshot saved: pic_${kind}.png")
        list(APPEND exrom_pic_tests debugger_mapper_exrom_${kind})
    endforeach()
    # the debugger's views: the background window of pic_chr1m_exattr and the pattern window of pic_split_8x16
    foreach(view exattr_view split_8x16_wpat)
        add_test(NAME debugger_mapper_exrom_${view}
            COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_BINARY_DIR}
                    $<TARGET_FILE:one-debugger> --batch --log ${CMAKE_CURRENT_BINARY_DIR}/exrom_${view}.log
                    -s ${CMAKE_CURRENT_SOURCE_DIR}/exrom/pic_${view}.txt
                    ${CMAKE_CURRENT_SOURCE_DIR}/exrom/pic_$<IF:$<STREQUAL:${view},exattr_view>,chr1m_exattr,split_8x16>.nes)
        set_tests_properties(debugger_mapper_exrom_${view} PROPERTIES LABELS smoke TIMEOUT 60)
        list(APPEND exrom_pic_tests debugger_mapper_exrom_${view})
    endforeach()
    add_test(NAME debugger_mapper_exrom_pics
        COMMAND ${CMAKE_COMMAND} -DPY=${Python3_EXECUTABLE} -DDIR=${CMAKE_CURRENT_BINARY_DIR} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/exrom/pic_check.cmake)
    set_tests_properties(debugger_mapper_exrom_pics PROPERTIES LABELS smoke TIMEOUT 60 DEPENDS "${exrom_pic_tests}")
endif()

# The MMC5's expansion audio against Mesen's recording of tests/exrom/mmc5audio.nes (test_roms/apu_reference/, not in the repository: skipped
# without it): the peak-to-peak level of every 25 ms block within 8 % (tests/mmc5_audio_check.py)
if(TARGET one AND Python3_FOUND AND EXISTS ${ONE_ROOT}/apu_reference/mmc5audio.wav)
    add_test(NAME apu_mmc5audio
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/mmc5_audio_check.py $<TARGET_FILE:one>
                ${CMAKE_CURRENT_SOURCE_DIR}/exrom/mmc5audio.nes ${ONE_ROOT}/apu_reference/mmc5audio.wav)
    set_tests_properties(apu_mmc5audio PROPERTIES LABELS smoke TIMEOUT 120)
endif()

# The Sunsoft 5B's audio (mapper 69) against Mesen's recording of Gimmick! (test_roms/apu_reference/ and commercial_games/ are not in the repository:
# skipped without them): the peak-to-peak level of every 25 ms block of the whole game section (tests/gimmick_audio_check.py)
if(TARGET one AND Python3_FOUND AND EXISTS ${ONE_ROOT}/apu_reference/gimmick.wav AND EXISTS "${ONE_ROOT}/commercial_games/Gimmick! (Japan).zip"
   AND EXISTS ${ONE_ROOT}/commercial_games/gimmick_input.txt)
    add_test(NAME apu_gimmick5b
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gimmick_audio_check.py $<TARGET_FILE:one>
                "${ONE_ROOT}/commercial_games/Gimmick! (Japan).zip" ${ONE_ROOT}/commercial_games/gimmick_input.txt
                ${ONE_ROOT}/apu_reference/gimmick.wav)
    set_tests_properties(apu_gimmick5b PROPERTIES LABELS smoke TIMEOUT 120)
endif()

# The VRC6's audio (mapper 24) against Mesen's recording of Akumajou Densetsu, the same way
if(TARGET one AND Python3_FOUND AND EXISTS ${ONE_ROOT}/apu_reference/akumajou.wav AND EXISTS "${ONE_ROOT}/commercial_games/Akumajou Densetsu (Japan).zip"
   AND EXISTS ${ONE_ROOT}/commercial_games/akumajou_input.txt)
    add_test(NAME apu_akumajou
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gimmick_audio_check.py $<TARGET_FILE:one>
                "${ONE_ROOT}/commercial_games/Akumajou Densetsu (Japan).zip" ${ONE_ROOT}/commercial_games/akumajou_input.txt
                ${ONE_ROOT}/apu_reference/akumajou.wav --cycles 77426804)
    set_tests_properties(apu_akumajou PROPERTIES LABELS smoke TIMEOUT 120)
endif()

# ... and of the VRC6b (mapper 26, Esper Dream 2), whose recording began from a save (--sav-in)
if(TARGET one AND Python3_FOUND AND EXISTS ${ONE_ROOT}/apu_reference/esperdream2.wav AND EXISTS "${ONE_ROOT}/commercial_games/Esper Dream 2 - Aratanaru Tatakai (Japan).zip"
   AND EXISTS ${ONE_ROOT}/commercial_games/esperdream2_input.txt AND EXISTS ${ONE_ROOT}/commercial_games/esperdream2_initial.sav)
    add_test(NAME apu_esperdream2
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gimmick_audio_check.py $<TARGET_FILE:one>
                "${ONE_ROOT}/commercial_games/Esper Dream 2 - Aratanaru Tatakai (Japan).zip" ${ONE_ROOT}/commercial_games/esperdream2_input.txt
                ${ONE_ROOT}/apu_reference/esperdream2.wav --cycles 68611796 --sav-in ${ONE_ROOT}/commercial_games/esperdream2_initial.sav)
    set_tests_properties(apu_esperdream2 PROPERTIES LABELS smoke TIMEOUT 120)
endif()

# The Namco 163's audio (mapper 19) against Mesen's recordings of Rolling Thunder and Final Lap
if(TARGET one AND Python3_FOUND AND EXISTS ${ONE_ROOT}/apu_reference/rollingthunder.wav AND EXISTS "${ONE_ROOT}/commercial_games/Rolling Thunder (Japan).zip"
   AND EXISTS ${ONE_ROOT}/commercial_games/rollingthunder_input.txt)
    add_test(NAME apu_rollingthunder
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gimmick_audio_check.py $<TARGET_FILE:one>
                "${ONE_ROOT}/commercial_games/Rolling Thunder (Japan).zip" ${ONE_ROOT}/commercial_games/rollingthunder_input.txt
                ${ONE_ROOT}/apu_reference/rollingthunder.wav --cycles 28623576)
    set_tests_properties(apu_rollingthunder PROPERTIES LABELS smoke TIMEOUT 120)
endif()
if(TARGET one AND Python3_FOUND AND EXISTS ${ONE_ROOT}/apu_reference/finallap.wav AND EXISTS "${ONE_ROOT}/commercial_games/Final Lap (Japan).zip"
   AND EXISTS ${ONE_ROOT}/commercial_games/finallap_input.txt)
    add_test(NAME apu_finallap
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gimmick_audio_check.py $<TARGET_FILE:one>
                "${ONE_ROOT}/commercial_games/Final Lap (Japan).zip" ${ONE_ROOT}/commercial_games/finallap_input.txt
                ${ONE_ROOT}/apu_reference/finallap.wav --cycles 17270193)
    set_tests_properties(apu_finallap PROPERTIES LABELS smoke TIMEOUT 120)
endif()

# NES-EVENT (mapper 105) timer: the IRQ must come 2^29 CPU cycles after the counter was last started (tests/nesevent/event.s); Mesen's Lua callback gives the
# same cycle minus 1 ($C002 reads 51 there, 52 here). About 45 s of emulation.
if(TARGET one)
    add_test(NAME nesevent_timer
        COMMAND $<TARGET_FILE:one> --headless --unthrottled --no-sav --lua ${CMAKE_CURRENT_SOURCE_DIR}/nesevent/irq_cycle.lua -k 700000000
                ${CMAKE_CURRENT_SOURCE_DIR}/nesevent/event.nes)
    set_tests_properties(nesevent_timer PROPERTIES LABELS "slow;nightly" TIMEOUT 600
        PASS_REGULAR_EXPRESSION "irq handler entered at cycle 536948900")
endif()

# The same for the PCM read mode (tests/exrom/mmc5pcmread.nes: a nonzero byte read at $8000-$BFFF becomes the output), when its
# recording is there
if(TARGET one AND Python3_FOUND AND EXISTS ${ONE_ROOT}/apu_reference/mmc5pcmread.wav)
    add_test(NAME apu_mmc5pcmread
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/mmc5_audio_check.py $<TARGET_FILE:one>
                ${CMAKE_CURRENT_SOURCE_DIR}/exrom/mmc5pcmread.nes ${ONE_ROOT}/apu_reference/mmc5pcmread.wav
                --seconds 8.3 --cycles 16000000)
    set_tests_properties(apu_mmc5pcmread PROPERTIES LABELS smoke TIMEOUT 120)
endif()

# Labels in a banked ROM (tests/banked: MMC1, ld65 debug info): a label names one bank's code, not a CPU address
if(TARGET one-debugger)
    add_test(NAME debugger_banked_labels
        COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_banked_labels.log
                -l ${CMAKE_CURRENT_SOURCE_DIR}/banked/banked.dbg
                -s ${CMAKE_CURRENT_SOURCE_DIR}/banked/labels.txt ${CMAKE_CURRENT_SOURCE_DIR}/banked/banked.nes)
    set_tests_properties(debugger_banked_labels PROPERTIES LABELS smoke TIMEOUT 60
        PASS_REGULAR_EXPRESSION "'bank2_func' is at PRG-ROM \\$08000, which is not mapped in right now"
        FAIL_REGULAR_EXPRESSION "ASSERTION FAILED|Unknown symbol")
    if(Python3_FOUND)
        # The remote channel on Windows (TCP only): one-debugger.exe under Wine, from a Linux host (remote_check.py --wine)
        if(WIN32 AND CMAKE_HOST_UNIX AND CMAKE_CROSSCOMPILING_EMULATOR MATCHES "wine")
            add_test(NAME debugger_remote_wine
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/remote_check.py --wine $<TARGET_FILE:one-debugger>
                        ${ONE_ROOT}/tests/data/nestest.nes ${CMAKE_CURRENT_SOURCE_DIR}/mmc1/mmc1.nes
                        ${CMAKE_CURRENT_SOURCE_DIR}/health/health.nes)
            set_tests_properties(debugger_remote_wine PROPERTIES LABELS smoke TIMEOUT 240
                PASS_REGULAR_EXPRESSION "remote channel ok" FAIL_REGULAR_EXPRESSION "FAILED")
        endif()
        # The remote channel (--remote, for the MCP adapter): token, refusals, allowlist, runs, pause, screenshot, TCP
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME debugger_remote
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/remote_check.py $<TARGET_FILE:one-debugger>
                        ${ONE_ROOT}/tests/data/nestest.nes ${CMAKE_CURRENT_SOURCE_DIR}/mmc1/mmc1.nes
                        ${CMAKE_CURRENT_SOURCE_DIR}/health/health.nes)
            set_tests_properties(debugger_remote PROPERTIES LABELS smoke TIMEOUT 120
                PASS_REGULAR_EXPRESSION "remote channel ok" FAIL_REGULAR_EXPRESSION "FAILED")
            # The MCP server (tools/mcp/one_mcp.py) through an MCP client; skipped without the MCP SDK (pip install mcp)
            add_test(NAME debugger_mcp
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/mcp_check.py $<TARGET_FILE:one-debugger>
                        ${ONE_ROOT}/tests/data/nestest.nes)
            set_tests_properties(debugger_mcp PROPERTIES LABELS smoke TIMEOUT 120 SKIP_RETURN_CODE 77
                FAIL_REGULAR_EXPRESSION "FAILED")
            # MCP phase 4: a scripted agent finds tests/health's health byte, its DEC and a working Game Genie code
            add_test(NAME debugger_mcp_health
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/health_check.py $<TARGET_FILE:one-debugger>
                        ${CMAKE_CURRENT_SOURCE_DIR}/health/health.nes)
            set_tests_properties(debugger_mcp_health PROPERTIES LABELS smoke TIMEOUT 180 SKIP_RETURN_CODE 77
                PASS_REGULAR_EXPRESSION "health ok" FAIL_REGULAR_EXPRESSION "FAILED")
        endif()
        # Memory search: filters typed one after another judge the same stretch of play (cs same + cs notwritten)
        add_test(NAME debugger_search_stretch
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/search_check.py $<TARGET_FILE:one-debugger>
                    ${CMAKE_CURRENT_SOURCE_DIR}/mmc1/mmc1.nes)
        set_tests_properties(debugger_search_stretch PROPERTIES LABELS smoke TIMEOUT 120
            PASS_REGULAR_EXPRESSION "filters on one stretch agree")
        # Screenshot: written by the 'screenshot' command, checked as a PNG (structure, size, a real picture)
        add_test(NAME debugger_screenshot
            COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_BINARY_DIR}
                    $<TARGET_FILE:one-debugger> --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_screenshot.log
                    -s ${CMAKE_CURRENT_SOURCE_DIR}/screenshot.txt ${ONE_ROOT}/tests/data/nestest.nes)
        set_tests_properties(debugger_screenshot PROPERTIES LABELS smoke TIMEOUT 60
            PASS_REGULAR_EXPRESSION "Screenshot saved: debugger_screenshot.png")
        add_test(NAME debugger_screenshot_png
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/png_check.py
                    ${CMAKE_CURRENT_BINARY_DIR}/debugger_screenshot.png)
        set_tests_properties(debugger_screenshot_png PROPERTIES LABELS smoke TIMEOUT 60
            DEPENDS debugger_screenshot)
        # Disable Sprite Limit: obj_over.nes has nine sprites on a line; off: eight, on: all nine, nothing else differs
        if(EXISTS ${ONE_ROOT}/test_roms/obj_over.nes)   # (third-party ROM: tools/fetch_test_roms.sh)
            add_test(NAME debugger_sprite_limit
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/sprite_limit_check.py $<TARGET_FILE:one-debugger>
                        ${ONE_ROOT}/test_roms/obj_over.nes ${CMAKE_CURRENT_BINARY_DIR})
            set_tests_properties(debugger_sprite_limit PROPERTIES LABELS smoke TIMEOUT 120
                PASS_REGULAR_EXPRESSION "sprite_limit: ok")
        endif()
        # The background view (wbg save, no window needed): nestest's menu after 30 frames, the part shown (scroll 0)
        # outlined, its mirror dimmed (tests/bg_check.py)
        set(wbg_script "")
        foreach(i RANGE 1 30)
            string(APPEND wbg_script "gsl 100\ngsl 200\n") # (one frame: gsl to the line it is on stops at once)
        endforeach()
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/wbg.txt "${wbg_script}wbg save debugger_wbg.png\n")
        add_test(NAME debugger_wbg
            COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_BINARY_DIR}
                    $<TARGET_FILE:one-debugger> --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_wbg.log
                    -s ${CMAKE_CURRENT_BINARY_DIR}/wbg.txt ${ONE_ROOT}/tests/data/nestest.nes)
        set_tests_properties(debugger_wbg PROPERTIES LABELS smoke TIMEOUT 60
            PASS_REGULAR_EXPRESSION "Background saved: debugger_wbg.png")
        add_test(NAME debugger_wbg_png
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/bg_check.py
                    ${CMAKE_CURRENT_BINARY_DIR}/debugger_wbg.png)
        set_tests_properties(debugger_wbg_png PROPERTIES LABELS smoke TIMEOUT 60 DEPENDS debugger_wbg)
    endif()
endif()

# The settings table (settings.h): defaults, reading one.cfg, bad values, saving changes in place
# (tests/settings_test.c). In a directory of its own, with XDG_CONFIG_HOME there: no real one.cfg is touched.
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/settings_dir)
add_executable(settings_test settings_test.c)
target_link_libraries(settings_test PRIVATE one_core)
add_test(NAME settings_table COMMAND settings_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/settings_dir)
set_tests_properties(settings_table PROPERTIES LABELS smoke TIMEOUT 60
    ENVIRONMENT "XDG_CONFIG_HOME=config;HOME=${CMAKE_CURRENT_BINARY_DIR}/settings_dir;APPDATA=config"
    PASS_REGULAR_EXPRESSION "settings_test: all passed")

# Game Genie codes remembered per game (tests/cheats_test.c): the cheats file written, read back by the game's CRC,
# another game empty; nestest's No-Intro CRC. In a directory of its own, with XDG_CONFIG_HOME there.
file(REMOVE_RECURSE ${CMAKE_CURRENT_BINARY_DIR}/cheats_dir)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/cheats_dir)
# (the user's own code database, which cheats.c reads from ONE's config directory: a made-up game)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/cheats_dir/config/one/cheats/nes_gamegenie.txt
    "# test database\n@1111111111111111111111111111111111111111 Test Game (USA)\nSXIOPO\tTest one\nAEUKZIGA\tTest two\n")
add_executable(cheats_test cheats_test.c)
target_link_libraries(cheats_test PRIVATE one_core)
add_test(NAME cheats_per_game COMMAND cheats_test ${ONE_ROOT}/tests/data/nestest.nes
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/cheats_dir)
set_tests_properties(cheats_per_game PROPERTIES LABELS smoke TIMEOUT 60
    ENVIRONMENT "XDG_CONFIG_HOME=config;HOME=${CMAKE_CURRENT_BINARY_DIR}/cheats_dir;APPDATA=config"
    PASS_REGULAR_EXPRESSION "cheats_test: all passed")

# Games inside .zip files (tests/zip_test.c; fixtures in tests/zip/): stored / deflate, damaged files
add_executable(zip_test zip_test.c)
target_link_libraries(zip_test PRIVATE one_core)
add_test(NAME zip_games COMMAND zip_test ${ONE_ROOT}/tests/data/nestest.nes ${ONE_ROOT}/tests/zip)
set_tests_properties(zip_games PROPERTIES LABELS smoke TIMEOUT 60 PASS_REGULAR_EXPRESSION "zip_test: all passed")

# The trace of a game out of a ZIP is the trace of the .nes (the same bytes)
add_test(NAME zip_trace COMMAND ${ONE_BASH} ${ONE_ROOT}/tests/zip_trace.sh $<TARGET_FILE:one> ${ONE_ROOT})
set_tests_properties(zip_trace PROPERTIES LABELS smoke TIMEOUT 60 PASS_REGULAR_EXPRESSION "zip_trace identical")

# Gamepads: SDL virtual gamepads stand in for real ones (tests/input_test.c); needs the SDL2 front end
if(ONE_SDL2_TARGET)
    add_executable(input_test input_test.c)
    target_link_libraries(input_test PRIVATE one_core)
    add_test(NAME input_gamepads COMMAND input_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(input_gamepads PROPERTIES LABELS smoke TIMEOUT 60 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
endif()

# The debugger's frame-in-progress view (rs): video_show_in_progress() dims what is not drawn yet, marks the beam
if(ONE_SDL2_TARGET)
    add_executable(frame_progress_test frame_progress_test.c)
    target_link_libraries(frame_progress_test PRIVATE one_core)
    add_test(NAME frame_in_progress COMMAND frame_progress_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(frame_in_progress PROPERTIES LABELS smoke TIMEOUT 60 ENVIRONMENT "SDL_VIDEODRIVER=dummy"
        PASS_REGULAR_EXPRESSION "frame_progress_test: all passed")
endif()

# The documented emulator settings file (made with one --write-config, into the build tree) loads without complaints
if(EXISTS ${ONE_ROOT}/tests/data/nestest.nes)
    # one --cheat: the code is applied from the first instruction (nestest's LDX #$00 at $C5F5 reads #$42)
    add_test(NAME one_cheat
        COMMAND one --headless -t -p C000 -k 20 --cheat ZKYGTI ${ONE_ROOT}/tests/data/nestest.nes)
    set_tests_properties(one_cheat PROPERTIES LABELS smoke TIMEOUT 60 PASS_REGULAR_EXPRESSION "C5F5  LDX #\\$42")
    add_test(NAME one_config_write COMMAND one --write-config ${CMAKE_CURRENT_BINARY_DIR}/one_test.cfg)
    set_tests_properties(one_config_write PROPERTIES LABELS smoke TIMEOUT 60 FIXTURES_SETUP one_test_cfg)
    add_test(NAME one_config
        COMMAND one --config ${CMAKE_CURRENT_BINARY_DIR}/one_test.cfg --headless -k 1000 ${ONE_ROOT}/tests/data/nestest.nes)
    set_tests_properties(one_config PROPERTIES LABELS smoke TIMEOUT 60 FIXTURES_REQUIRED one_test_cfg
        PASS_REGULAR_EXPRESSION "Settings from" FAIL_REGULAR_EXPRESSION "one\\.cfg:[0-9]+:")
endif()

# Lua scripts (script.c, Mesen 2 API; lua/fceux.lua, FCEUX API), each in 'one' and in one-debugger (where the
# script runs from the start and emu.stop() quits):
#   lua_api    events, memory types and state
#   lua_api2   drawing, input, memory callbacks, save states
#   lua_fceux  an FCEUX style script: main loop with emu.frameadvance(), memory/gui/joypad/savestate/rom/sound
#   lua_window one.openWindow and the other window functions (as pictures: headless), one.features()
if(EXISTS ${ONE_ROOT}/tests/mmc1/mmc1.nes)
    if(TARGET one-debugger)
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/lua_run.txt "x\n")
    endif()
    foreach(script lua_api lua_api2 lua_fceux lua_window)
        add_test(NAME ${script}
            COMMAND one --headless -k 10000000 --lua ${CMAKE_CURRENT_SOURCE_DIR}/${script}.lua
                    ${ONE_ROOT}/tests/mmc1/mmc1.nes
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(${script} PROPERTIES LABELS smoke TIMEOUT 60
            PASS_REGULAR_EXPRESSION "${script}: PASS" FAIL_REGULAR_EXPRESSION "${script}: FAIL|error:"
            ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/lua-data")
        if(TARGET one-debugger)
            add_test(NAME debugger_${script}
                COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_${script}.log
                        --lua ${CMAKE_CURRENT_SOURCE_DIR}/${script}.lua -s ${CMAKE_CURRENT_BINARY_DIR}/lua_run.txt
                        ${ONE_ROOT}/tests/mmc1/mmc1.nes
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
            set_tests_properties(debugger_${script} PROPERTIES LABELS smoke TIMEOUT 60
                PASS_REGULAR_EXPRESSION "${script}: PASS" FAIL_REGULAR_EXPRESSION "${script}: FAIL|error:"
                ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/lua-data")
        endif()
    endforeach()
    # lua/examples/lagcounter.lua counts the same lag frames as ONE's own counter (tests/lua_lag.lua, tests/lag/lag.nes)
    # emu.setInput({x=, y=, trigger=}, 1) aims the Zapper (tests/lua_zapper.lua, tests/zapper/zapper_free.nes)
    add_test(NAME lua_zapper
        COMMAND one --headless -k 10000000 --lua ${CMAKE_CURRENT_SOURCE_DIR}/lua_zapper.lua ${ONE_ROOT}/tests/zapper/zapper_free.nes)
    set_tests_properties(lua_zapper PROPERTIES LABELS smoke TIMEOUT 60
        PASS_REGULAR_EXPRESSION "lua_zapper: PASS" FAIL_REGULAR_EXPRESSION "lua_zapper: FAIL|error:")
    add_test(NAME lua_lag
        COMMAND one --headless -k 10000000 --lua ${CMAKE_CURRENT_SOURCE_DIR}/lua_lag.lua ${ONE_ROOT}/tests/lag/lag.nes
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(lua_lag PROPERTIES LABELS smoke TIMEOUT 60
        PASS_REGULAR_EXPRESSION "lua_lag: PASS" FAIL_REGULAR_EXPRESSION "lua_lag: FAIL|error:"
        ENVIRONMENT "EXAMPLE_LUA=${ONE_ROOT}/lua/examples/lagcounter.lua;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/lua-data")
    if(TARGET one-debugger)
        add_test(NAME debugger_lua_lag
            COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_lua_lag.log
                    --lua ${CMAKE_CURRENT_SOURCE_DIR}/lua_lag.lua -s ${CMAKE_CURRENT_BINARY_DIR}/lua_run.txt
                    ${ONE_ROOT}/tests/lag/lag.nes
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(debugger_lua_lag PROPERTIES LABELS smoke TIMEOUT 60
            PASS_REGULAR_EXPRESSION "lua_lag: PASS" FAIL_REGULAR_EXPRESSION "lua_lag: FAIL|error:"
            ENVIRONMENT "EXAMPLE_LUA=${ONE_ROOT}/lua/examples/lagcounter.lua;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/lua-data")
    endif()
    # The example scripts in lua/examples on nestest (tests/lua_example.lua): each runs in one and saves its window;
    # in one-debugger its picture must be the same as the debugger's own window's (tests/png_same.py):
    #   background.lua (scroll rebuilt from register writes) against wbg -- in one also checked like wbg's
    #   (tests/bg_check.py); palette.lua against wpal; pattern.lua against wpat
    if(EXISTS ${ONE_ROOT}/tests/data/nestest.nes AND Python3_FOUND)
        foreach(pair background:wbg palette:wpal pattern:wpat)
            string(REPLACE ":" ";" pair "${pair}")
            list(GET pair 0 example)
            list(GET pair 1 viewer)
            set(env "EXAMPLE=${example};VIEWER=${viewer};EXAMPLE_LUA=${ONE_ROOT}/lua/examples/${example}.lua"
                    "TEST_OUT=${CMAKE_CURRENT_BINARY_DIR}")
            add_test(NAME lua_${example}
                COMMAND one --headless -k 10000000 --lua ${CMAKE_CURRENT_SOURCE_DIR}/lua_example.lua
                        ${ONE_ROOT}/tests/data/nestest.nes)
            set_tests_properties(lua_${example} PROPERTIES LABELS smoke TIMEOUT 60 ENVIRONMENT "${env}"
                PASS_REGULAR_EXPRESSION "lua_${example}: saved" FAIL_REGULAR_EXPRESSION "FAIL|error:")
            set(after lua_${example})
            if(example STREQUAL "background")
                add_test(NAME lua_background_png
                    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/bg_check.py
                            ${CMAKE_CURRENT_BINARY_DIR}/lua_background.png)
                set_tests_properties(lua_background_png PROPERTIES LABELS smoke TIMEOUT 60 DEPENDS lua_background)
                set(after lua_background_png)
            endif()
            if(TARGET one-debugger)
                add_test(NAME debugger_lua_${example}
                    COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_lua_${example}.log
                            --lua ${CMAKE_CURRENT_SOURCE_DIR}/lua_example.lua
                            -s ${CMAKE_CURRENT_BINARY_DIR}/lua_run.txt ${ONE_ROOT}/tests/data/nestest.nes)
                set_tests_properties(debugger_lua_${example} PROPERTIES LABELS smoke TIMEOUT 60 ENVIRONMENT "${env}"
                    PASS_REGULAR_EXPRESSION "lua_${example}: saved" FAIL_REGULAR_EXPRESSION "FAIL|error:"
                    DEPENDS ${after}) # (the same output file: one after the other)
                add_test(NAME debugger_lua_${example}_same
                    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/png_same.py
                            ${CMAKE_CURRENT_BINARY_DIR}/lua_${example}.png
                            ${CMAKE_CURRENT_BINARY_DIR}/${viewer}_${example}.png)
                set_tests_properties(debugger_lua_${example}_same PROPERTIES LABELS smoke TIMEOUT 60
                    DEPENDS debugger_lua_${example})
            endif()
        endforeach()
    endif()
    # the lua command's file names: quoted or not, with spaces, Tab completion, lua stop N
    if(TARGET one-debugger)
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/lua files/hello world.lua" "print('hello world ran')\n")
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/lua_files.txt
            "lua \"lua files/hello world.lua\"\n"
            "lua lua files/hello world.lua\n"
            "lua stop 1\n"
            "lua stop 0\n"
            "!complete \"lua lua f\" \"lua lua files/\"\n"
            "!complete \"lua lua files/h\" \"lua lua files/hello world.lua\"\n"
            "!complete \"source lua files/h\" \"source lua files/hello world.lua\"\n"
            # nothing typed after the command yet (it used to throw): every file and directory, nothing in common
            "!complete \"lua \" \"lua \"\n"
            "!complete \"load  \" \"load  \"\n")
        add_test(NAME debugger_lua_files
            COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_lua_files.log
                    -s ${CMAKE_CURRENT_BINARY_DIR}/lua_files.txt ${ONE_ROOT}/tests/mmc1/mmc1.nes
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(debugger_lua_files PROPERTIES LABELS smoke TIMEOUT 60
            PASS_REGULAR_EXPRESSION "hello world ran[^#]*hello world ran"
            FAIL_REGULAR_EXPRESSION "ASSERTION FAILED|error:|No file|No Lua script")
    endif()
    # debugger scripting: debugger.command(), bpx ... if / do, source, emu.breakExecution(), emu.getLabelAddress()
    if(TARGET one-debugger)
        add_test(NAME debugger_scripting
            COMMAND one-debugger --batch --log ${CMAKE_CURRENT_BINARY_DIR}/debugger_scripting.log
                    --lua ${CMAKE_CURRENT_SOURCE_DIR}/debugger_scripting.lua -s ${CMAKE_CURRENT_BINARY_DIR}/lua_run.txt
                    ${ONE_ROOT}/tests/mmc1/mmc1.nes
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(debugger_scripting PROPERTIES LABELS smoke TIMEOUT 60
            PASS_REGULAR_EXPRESSION "debugger_scripting: PASS"
            FAIL_REGULAR_EXPRESSION "debugger_scripting: FAIL|error:"
            ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/lua-data")
    endif()
endif()

# Sound (ctest -L audio, about half a minute): blargg's apu_mixer ROMs must play near silence between their beeps,
# and where a Mesen APU log of the ROM is in test_roms/apu_reference/ (tools/mesen/README.md), ONE's must match it.
# square/triangle/dmc only: noise.nes fades noise in and out on purpose.
if(Python3_FOUND)
    # ROM, seconds to run (to just after the second beep), loudest allowed between the beeps
    foreach(test square:18:0.06 triangle:12:0.04 dmc:14:0.04)
        string(REPLACE ":" ";" test "${test}")
        list(GET test 0 rom)
        list(GET test 1 seconds)
        list(GET test 2 limit)
        set(rom_path ${ONE_ROOT}/test_roms/apu_mixer/${rom}.nes)
        if(NOT EXISTS ${rom_path})
            continue()
        endif()
        math(EXPR cycles "${seconds} * 1789773")
        set(reference_args)
        if(EXISTS ${ONE_ROOT}/apu_reference/apu_${rom}.txt)
            set(reference_args -DREFERENCE=${ONE_ROOT}/apu_reference/apu_${rom}.txt)
        endif()
        add_test(NAME audio_mixer_${rom}
            COMMAND ${CMAKE_COMMAND} -DONE=$<TARGET_FILE:one> -DPYTHON=${Python3_EXECUTABLE}
                    -DTOOLS=${ONE_ROOT}/tools -DROM=${rom_path} -DCYCLES=${cycles}
                    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/audio_${rom} -DLIMIT=${limit} ${reference_args}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_audio_test.cmake)
        set_tests_properties(audio_mixer_${rom} PROPERTIES LABELS audio TIMEOUT 300)
    endforeach()
endif()

# Save states. Every variable of the core must be in a save state or marked as not machine state (lint: needs
# clang 18). Round trips (Python 3): a state saved during a run and loaded into a fresh emulator continues exactly
# like the original run (CPU trace, APU log, and the whole machine 20 frames later; tests/state_roundtrip.py).
if(Python3_FOUND)
    add_test(NAME savestate_fields COMMAND ${Python3_EXECUTABLE} ${ONE_ROOT}/tools/state_check.py -I ${ONE_GENERATED_DIR})
    set_tests_properties(savestate_fields PROPERTIES LABELS lint TIMEOUT 300 SKIP_RETURN_CODE 2)
    # ROM, frame(s) to save at, CPU cycles to run, labels. At least one ROM per mapper: NROM (nestest, dmc),
    # MMC1 (tests/mmc1, apu_test), CNROM (tests/cnrom). tests/mmc1 and tests/cnrom keep every mapper register in use;
    # their save frames were chosen so that leaving any one mapper variable out of the state fails the test.
    foreach(test "nestest:tests/data/nestest.nes:61:3000000:smoke;savestate"
                 "mmc1:tests/mmc1/mmc1.nes:30,31,43:2200000:smoke;savestate"
                 "cnrom:tests/cnrom/cnrom.nes:61,63:3000000:smoke;savestate"
                 "exrom:tests/exrom/exrom.nes:5,8:3000000:smoke;savestate"
                 "dmc:test_roms/apu_mixer/dmc.nes:300:12000000:savestate"
                 "apu_test:test_roms/apu_test/apu_test.nes:200:10000000:savestate")
        string(REPLACE ":" ";" test "${test}")
        list(GET test 0 name)
        list(GET test 1 rom)
        list(GET test 2 frame)
        list(GET test 3 cycles)
        list(SUBLIST test 4 -1 labels)
        if(NOT EXISTS ${ONE_ROOT}/${rom})
            continue()
        endif()
        add_test(NAME savestate_roundtrip_${name}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/state_roundtrip.py $<TARGET_FILE:one>
                    ${ONE_ROOT}/${rom} ${frame} ${cycles} ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(savestate_roundtrip_${name} PROPERTIES LABELS "${labels}" TIMEOUT 300)
    endforeach()
endif()

# Console output goes through log.h (tools/print_check.py): no printf/fprintf(stderr) in the core outside the functions
# that print what the user asked for
if(Python3_FOUND)
    add_test(NAME console_output COMMAND ${Python3_EXECUTABLE} ${ONE_ROOT}/tools/print_check.py)
    set_tests_properties(console_output PROPERTIES LABELS "smoke;lint" TIMEOUT 60)
    # one version everywhere: project(VERSION) = one --version = one-debugger --version = the libretro core's = its .info file
    set(version_args --project-version ${PROJECT_VERSION} --one $<TARGET_FILE:one>
                     --info ${ONE_ROOT}/libretro/one_libretro.info)
    if(TARGET one-debugger)
        list(APPEND version_args --debugger $<TARGET_FILE:one-debugger>)
    endif()
    if(TARGET one_libretro)
        list(APPEND version_args --libretro-host $<TARGET_FILE:libretro_host> --core $<TARGET_FILE:one_libretro>)
    endif()
    add_test(NAME version_consistency COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/version_check.py ${version_args})
    set_tests_properties(version_consistency PROPERTIES LABELS "smoke;lint" TIMEOUT 60)
    # the menu (gui/) uses the emulator through frontend.h only, and the core includes nothing of the menu
    add_test(NAME gui_separation COMMAND ${Python3_EXECUTABLE} ${ONE_ROOT}/tools/separation_check.py)
    set_tests_properties(gui_separation PROPERTIES LABELS "smoke;lint" TIMEOUT 60)
endif()

# The libretro core (libretro/): a minimal frontend loads one_libretro.so, runs a game, checks picture, sound and
# load / unload / load (tests/libretro_host.c)
if(ONE_BUILD_LIBRETRO)
    add_executable(libretro_host libretro_host.c)
    if(NOT WIN32)
        target_link_libraries(libretro_host PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME libretro_core COMMAND libretro_host $<TARGET_FILE:one_libretro> ${ONE_ROOT}/tests/data/nestest.nes
             --zapper ${ONE_ROOT}/tests/zapper/zapper_free.nes)
    set_tests_properties(libretro_core PROPERTIES LABELS "smoke;libretro" TIMEOUT 120
        PASS_REGULAR_EXPRESSION "libretro_host: all passed")
endif()

# PPU catch-up (FEATURE_PPU_CATCHUP): with and without it the whole machine must be the same at the same frame. The
# ppu_catchup test uses a selection of the test ROMs (and the commercial games when they are there); ppu_catchup_all (label
# catchup, not smoke) every ROM of test_roms/. Needs the feature in 'one' (ONE_PPU_CATCHUP).
if(ONE_PPU_CATCHUP AND Python3_FOUND)
    add_test(NAME ppu_catchup COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ppu_catchup_check.py $<TARGET_FILE:one>)
    # (not smoke: it takes many minutes, more on the 4-core CI machines and under a sanitizer; check.sh full runs it)
    set_tests_properties(ppu_catchup PROPERTIES LABELS "catchup;slow" PROCESSORS 8 TIMEOUT 900
        PASS_REGULAR_EXPRESSION "ppu_catchup_check: [1-9][0-9]* identical, 0 different")
    add_test(NAME ppu_catchup_all COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ppu_catchup_check.py $<TARGET_FILE:one> --all)
    set_tests_properties(ppu_catchup_all PROPERTIES LABELS "catchup;slow;nightly" PROCESSORS 8 TIMEOUT 3600
        PASS_REGULAR_EXPRESSION "ppu_catchup_check: [1-9][0-9]* identical, 0 different")
endif()

# Every test runs with the hardware's sprite limit, whatever the user's one.cfg says (video.disable_sprite_limit):
# ONE_HARDWARE_SPRITES makes the PPU ignore the setting. The one test of the setting itself removes it again.
get_property(one_all_tests DIRECTORY PROPERTY TESTS)
foreach(t ${one_all_tests})
    if(NOT t STREQUAL "debugger_sprite_limit")
        set_property(TEST ${t} APPEND PROPERTY ENVIRONMENT "ONE_HARDWARE_SPRITES=1")
    endif()
endforeach()
