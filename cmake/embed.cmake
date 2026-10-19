# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# cmake -DINPUT=file -DOUTPUT=header.h -DNAME=symbol -P embed.cmake
# Writes header.h with 'static const char NAME[] = {...};' holding the file's bytes and a terminating 0, and
# 'NAME_size' (without the 0). Used for lua/fceux.lua, so the binaries need no data files at run time.
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" length)
math(EXPR size "${length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x..,){16})" "\\1\n    " bytes "${bytes}")
get_filename_component(source "${INPUT}" NAME)
file(WRITE "${OUTPUT}.tmp"
    "// generated from ${source} by cmake/embed.cmake: do not edit\n"
    "static const unsigned char ${NAME}[] = {\n    ${bytes}0};\n"
    "static const unsigned long ${NAME}_size = ${size};\n")
configure_file("${OUTPUT}.tmp" "${OUTPUT}" COPYONLY)   # (touches it only when it changed)
file(REMOVE "${OUTPUT}.tmp")
