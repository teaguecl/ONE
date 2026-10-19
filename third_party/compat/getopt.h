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
/*
 * Minimal getopt / getopt_long for platforms without one (MSVC).
 * Supports short options ("ab:c"), long options with no_argument / required_argument / optional_argument,
 * "--opt=value" and "--opt value", "-bvalue" and "-b value", "--" to end option parsing, and GNU-style
 * permutation (options may come after the ROM file name; non-options end up at argv[optind...]).
 * Only built when the toolchain has no <getopt.h> (see CMakeLists.txt).
 */
#ifndef ONE_COMPAT_GETOPT_H
#define ONE_COMPAT_GETOPT_H

#ifdef __cplusplus
extern "C" {
#endif

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

#define no_argument        0
#define required_argument  1
#define optional_argument  2

struct option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

int getopt(int argc, char *const argv[], const char *optstring);
int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex);

#ifdef __cplusplus
}
#endif

#endif /* ONE_COMPAT_GETOPT_H */
