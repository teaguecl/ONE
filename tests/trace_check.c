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
 * trace_check OUTPUT LOG
 *
 * Compares the trace lines in OUTPUT (everything ONE printed to stderr; only lines that start with a 4-digit hex
 * address followed by two spaces are trace lines) with a Mesen trace log, line by line. Line endings are ignored.
 * Exit status 0 = identical, 1 = different (first difference printed), 2 = usage / file error.
 * Portable replacement for "grep ... | cmp -" so the trace tests also run on Windows.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MAX_LINE 4096

static int is_trace_line(const char *s)
{
    for (int i = 0; i < 4; i++) {
        if (!isxdigit((unsigned char)s[i]) || islower((unsigned char)s[i])) {
            return 0;
        }
    }
    return (s[4] == ' ') && (s[5] == ' ');
}

static void chomp(char *s)
{
    size_t n = strlen(s);
    while ((n > 0) && ((s[n - 1] == '\n') || (s[n - 1] == '\r'))) {
        s[--n] = '\0';
    }
}

/* next trace line from f into buf; 0 at end of file */
static int next_trace_line(FILE *f, char *buf, int filter)
{
    while (fgets(buf, MAX_LINE, f) != NULL) {
        chomp(buf);
        if (!filter || is_trace_line(buf)) {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    static char a[MAX_LINE], b[MAX_LINE];
    long line = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: trace_check OUTPUT MESEN_LOG\n");
        return 2;
    }
    FILE *out = fopen(argv[1], "rb");
    FILE *log = fopen(argv[2], "rb");
    if ((out == NULL) || (log == NULL)) {
        fprintf(stderr, "trace_check: cannot open %s\n", (out == NULL) ? argv[1] : argv[2]);
        return 2;
    }
    for (;;) {
        int has_a = next_trace_line(out, a, 1);
        int has_b = next_trace_line(log, b, 0);
        line++;
        if (!has_a && !has_b) {
            printf("IDENTICAL (%ld lines)\n", line - 1);
            return 0;
        }
        if (!has_a || !has_b || (strcmp(a, b) != 0)) {
            printf("DIFFERS at line %ld\n  mesen: %s\n  one:   %s\n", line, has_b ? b : "<end of log>",
                   has_a ? a : "<end of trace>");
            return 1;
        }
    }
}
