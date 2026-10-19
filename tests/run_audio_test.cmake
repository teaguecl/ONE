# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# cmake -DONE=<one> -DPYTHON=<python3> -DTOOLS=<tools dir> -DROM=<rom> -DCYCLES=<n> -DOUT=<prefix>
#       [-DLIMIT=<ratio>] [-DREFERENCE=<Mesen apu_log.lua log>] -P run_audio_test.cmake
# Runs ONE headless from power-on for CYCLES CPU cycles, writing the sound (OUT.wav) and the APU state log
# (OUT.txt). With LIMIT, the sound between the ROM's two beeps must be at most LIMIT x as loud as the beeps
# (tools/audio_check.py); with REFERENCE, the APU log must match Mesen's (tools/apu_compare.py).
foreach(v ONE PYTHON TOOLS ROM CYCLES OUT)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "run_audio_test.cmake: ${v} not set")
    endif()
endforeach()

execute_process(
    COMMAND "${ONE}" --headless --unthrottled --wav "${OUT}.wav" --apu-log "${OUT}.txt" -k ${CYCLES} "${ROM}"
    OUTPUT_QUIET ERROR_QUIET
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "one exited with status ${rc}")
endif()
set(failed FALSE)
if(DEFINED LIMIT)
    execute_process(COMMAND "${PYTHON}" "${TOOLS}/audio_check.py" "${OUT}.wav" --quiet-between-beeps ${LIMIT}
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        set(failed TRUE)
    endif()
endif()
if(DEFINED REFERENCE)
    execute_process(COMMAND "${PYTHON}" "${TOOLS}/apu_compare.py" "${REFERENCE}" "${OUT}.txt" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        set(failed TRUE)
    endif()
endif()
if(failed)
    message(FATAL_ERROR "audio check failed")
endif()
