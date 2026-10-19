/*
  SPDX-License-Identifier: GPL-2.0-or-later
  ONE NES emulator
  Copyright (C) 1999-2026 Chris Teague

  ONE is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  ONE is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with ONE; if not, see <https://www.gnu.org/licenses/>.
*/
// Test of the bundled getopt_long (third_party/compat/getopt.c), which MSVC builds of `one` use: the same cases run
// against it (getopt_compat) and against the platform's getopt_long (getopt_system), and both must give the
// expected result, which is what GNU getopt does. Each case prints the options in order ("c=cfg" is -c cfg, "k" is
// -k) and, after "|", the arguments left over (the ROM name). Errors are '?' (opterr is 0: no messages).
#ifdef GETOPT_TEST_COMPAT
#include "getopt.h" // third_party/compat (first on the include path for this target)
#else
#include <getopt.h>
#endif
#include <stdio.h>
#include <string.h>

static const char short_options[] = "hdc:k:vVp:R:r:S:D:Htu";
static const struct option long_options[] = {{"help", no_argument, NULL, 1000},
                                             {"config", required_argument, NULL, 1001},
                                             {"wav", required_argument, NULL, 1002},
                                             {"no-sav", no_argument, NULL, 1003},
                                             {"lua", required_argument, NULL, 1004},
                                             {"opt", optional_argument, NULL, 1005},
                                             {NULL, 0, NULL, 0}};

typedef struct {
    int argc;
    const char *argv[8];
    const char *expected;
} getopt_case;

static const getopt_case cases[] = {{2, {"one", "game.nes"}, "| game.nes"},
                                    {4, {"one", "-h", "-d", "game.nes"}, "h d | game.nes"},
                                    {2, {"one", "-hd"}, "h d |"},
                                    {5, {"one", "-c", "cfg", "-k100", "game.nes"}, "c=cfg k=100 | game.nes"},
                                    {3, {"one", "game.nes", "-v"}, "v | game.nes"},
                                    {5, {"one", "game.nes", "--wav", "out.wav", "-d"}, "wav=out.wav d | game.nes"},
                                    {4, {"one", "g", "-c", "cfg"}, "c=cfg | g"},
                                    {4, {"one", "--wav=out.wav", "--no-sav", "g"}, "wav=out.wav no-sav | g"},
                                    {4, {"one", "--", "-d", "g"}, "| -d g"},
                                    {3, {"one", "-", "g"}, "| - g"},
                                    {3, {"one", "-d", "--"}, "d |"},
                                    {2, {"one", "-x"}, "? |"},
                                    {2, {"one", "--bogus"}, "? |"},
                                    {2, {"one", "-c"}, "? |"},
                                    {2, {"one", "--lua"}, "? |"},
                                    {2, {"one", "--no-sav=1"}, "? |"},
                                    {3, {"one", "-c", "-d"}, "c=-d |"},
                                    {3, {"one", "--lua", "-d"}, "lua=-d |"},
                                    {4, {"one", "--opt=5", "--opt", "g"}, "opt=5 opt | g"},
                                    {4, {"one", "-hdc", "cfg", "g"}, "h d c=cfg | g"},
                                    {3, {"one", "-S", "12"}, "S=12 |"},
                                    {0, {NULL}, NULL}};

static int run_case(const getopt_case *c)
{
    const char *argv[8] = {NULL}; // (the unused entries stay NULL: argv[argc] is NULL, and some getopts look at it)
    char text[256] = "";
    size_t n = 0;
    int option;
    int longindex = -1;
    int ok;

    for (int i = 0; i < c->argc; i++) {
        argv[i] = c->argv[i]; // (getopt_long permutes argv)
    }
    opterr = 0;
    optind = 0; // restart the scan
    while ((option = getopt_long(c->argc, (char *const *)argv, short_options, long_options, &longindex)) != -1) {
        const char *name;
        char shortname[2] = {(char)option, '\0'};
        if (option == '?') {
            name = "?";
        } else if (option >= 1000) {
            name = long_options[option - 1000].name;
        } else {
            name = shortname;
        }
        n += (size_t)snprintf(text + n, sizeof text - n, "%s%s%s%s", n ? " " : "", name, optarg ? "=" : "",
                              optarg ? optarg : "");
    }
    n += (size_t)snprintf(text + n, sizeof text - n, "%s|", n ? " " : "");
    for (int i = optind; i < c->argc; i++) {
        n += (size_t)snprintf(text + n, sizeof text - n, " %s", argv[i]);
    }
    ok = strcmp(text, c->expected) == 0;
    if (!ok) {
        printf("FAIL:");
        for (int i = 1; i < c->argc; i++) {
            printf(" %s", c->argv[i]);
        }
        printf("\n  expected: %s\n  got:      %s\n", c->expected, text);
    }
    return ok;
}

int main(void)
{
    int failures = 0;
    for (const getopt_case *c = cases; c->argc != 0; c++) {
        failures += !run_case(c);
    }
    printf("getopt_test: %s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
