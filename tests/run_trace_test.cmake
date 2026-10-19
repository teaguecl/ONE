# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# cmake -DONE=<one> -DTRACE_CHECK=<trace_check> -DROM=<rom> -DLOG=<mesen log> -DARGS="<extra args>" -DOUT=<tmp>
#       -P run_trace_test.cmake
# Runs ONE headless up to the last cycle of the Mesen log and compares the traces.
foreach(v ONE TRACE_CHECK ROM LOG OUT)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "run_trace_test.cmake: ${v} not set")
    endif()
endforeach()

# the cycle count to stop at is in the last line of the log ("... Cycle:NNN ...")
file(SIZE "${LOG}" log_size)
set(tail_offset 0)
if(log_size GREATER 4096)
    math(EXPR tail_offset "${log_size} - 4096")
endif()
file(READ "${LOG}" log_tail OFFSET ${tail_offset})
string(REGEX MATCHALL "Cycle:[0-9]+" cycles "${log_tail}")
list(GET cycles -1 last)
string(REPLACE "Cycle:" "" last "${last}")

separate_arguments(extra NATIVE_COMMAND "${ARGS}")
execute_process(
    COMMAND "${ONE}" --headless -t ${extra} -k ${last} "${ROM}"
    OUTPUT_QUIET
    ERROR_FILE "${OUT}"
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "one exited with status ${rc}")
endif()
execute_process(COMMAND "${TRACE_CHECK}" "${OUT}" "${LOG}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "trace differs from ${LOG}")
endif()
