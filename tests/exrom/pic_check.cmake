# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# ctest debugger_mapper_exrom_pics: runs pic_check.py on the screenshots of the three pic_*.nes tests
execute_process(COMMAND ${PY} ${SRC}/exrom/pic_check.py exattr ${DIR}/pic_exattr.png RESULT_VARIABLE r1)
execute_process(COMMAND ${PY} ${SRC}/exrom/pic_check.py split ${DIR}/pic_split_left.png ${DIR}/pic_split_right.png RESULT_VARIABLE r2)
execute_process(COMMAND ${PY} ${SRC}/exrom/pic_check.py same ${DIR}/pic_split_left.png ${SRC}/exrom/pic_split_left_mesen.png RESULT_VARIABLE r3)
execute_process(COMMAND ${PY} ${SRC}/exrom/pic_check.py same ${DIR}/pic_split_right.png ${SRC}/exrom/pic_split_right_mesen.png RESULT_VARIABLE r4)
if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR NOT r3 EQUAL 0 OR NOT r4 EQUAL 0)
    message(FATAL_ERROR "pic_check failed (exattr: ${r1}, split: ${r2}, Mesen left: ${r3}, right: ${r4})")
endif()
# the corners: every pixel like Mesen's screenshot, for the pictures that have one (tests/exrom/pic_<name>_mesen.png)
foreach(kind exattr split_scroll_a split_scroll_b exattr_scroll chr1m chr1m_exattr split_8x16 split_mode1 split_wrap chr1m_mode0
             chr1m_mode1 chr1m_mode2)
    if(EXISTS ${SRC}/exrom/pic_${kind}_mesen.png)
        execute_process(COMMAND ${PY} ${SRC}/exrom/pic_check.py same ${DIR}/pic_${kind}.png ${SRC}/exrom/pic_${kind}_mesen.png
                        RESULT_VARIABLE r)
        if(NOT r EQUAL 0)
            message(FATAL_ERROR "pic_${kind}: not like Mesen's screenshot")
        endif()
    endif()
endforeach()
# the debugger's views
execute_process(COMMAND ${PY} ${SRC}/exrom/pic_check.py view ${DIR}/pic_exattr_wbg.png ${DIR}/pic_exattr_view.png RESULT_VARIABLE r)
if(NOT r EQUAL 0)
    message(FATAL_ERROR "the background window does not show pic_exattr")
endif()
execute_process(COMMAND ${PY} ${SRC}/exrom/pic_check.py tables ${DIR}/pic_split_8x16_wpat.png RESULT_VARIABLE r)
if(NOT r EQUAL 0)
    message(FATAL_ERROR "the pattern window of pic_split_8x16: the tables are alike")
endif()
