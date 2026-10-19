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
 * Minimal getopt / getopt_long for platforms without one (MSVC). See getopt.h.
 */
#include <stdio.h>
#include <string.h>
#include "getopt.h"

char *optarg = NULL;
int optind = 1;
int opterr = 1;
int optopt = 0;

static int next_char = 0;   /* position inside a group of short options ("-abc"); 0 = start a new argv element */

static int is_option(const char *arg)
{
    return (arg != NULL) && (arg[0] == '-') && (arg[1] != '\0');
}

/* Move the option at argv[from] to argv[to], shifting argv[to..from-1] up by one (GNU-style permutation). */
static void move_to(char **argv, int from, int to)
{
    char *opt = argv[from];
    for (int i = from; i > to; i--) {
        argv[i] = argv[i - 1];
    }
    argv[to] = opt;
}

/* Does the option element 'arg' take its argument from the following argv element? */
static int needs_separate_arg(const char *arg, const char *optstring, const struct option *longopts)
{
    if ((arg[1] == '-') && (arg[2] != '\0')) {          /* --name or --name=value */
        const char *name = arg + 2;
        if (strchr(name, '=') != NULL) {
            return 0;
        }
        for (int i = 0; longopts && longopts[i].name; i++) {
            if (strcmp(longopts[i].name, name) == 0) {
                return longopts[i].has_arg == required_argument;
            }
        }
        return 0;
    }
    for (int k = 1; arg[k] != '\0'; k++) {              /* -abc: the first option with an argument ends the group */
        const char *spec = (arg[k] != ':') ? strchr(optstring, arg[k]) : NULL;
        if ((spec != NULL) && (spec[1] == ':')) {
            return (arg[k + 1] == '\0') && (spec[2] != ':');
        }
    }
    return 0;
}

/* Bring the next option (and its separate argument, if any) to argv[optind]. Returns 0 when there are no more
 * options; the non-options then sit at argv[optind...]. */
static int find_next_option(int argc, char **argv, const char *optstring, const struct option *longopts)
{
    int i = optind;
    while ((i < argc) && !is_option(argv[i])) {
        i++;
    }
    if (i >= argc) {
        return 0;
    }
    if (strcmp(argv[i], "--") == 0) {
        move_to(argv, i, optind);       /* "--" ends option parsing; skip it */
        optind++;
        return 0;
    }
    if (i != optind) {
        int with_arg = needs_separate_arg(argv[i], optstring, longopts) && (i + 1 < argc);
        move_to(argv, i, optind);
        if (with_arg) {
            move_to(argv, i + 1, optind + 1);
        }
    }
    return 1;
}

/* After an option at argv[optind-1] consumed the following element as its argument, keep both together. */
static void take_separate_argument(int argc, char **argv)
{
    if (optind < argc) {
        optarg = argv[optind++];
    }
}

static int parse_long(int argc, char **argv, const struct option *longopts, int *longindex)
{
    char *arg = argv[optind] + 2;
    const char *eq = strchr(arg, '=');
    size_t len = eq ? (size_t)(eq - arg) : strlen(arg);
    int match = -1;

    optind++;
    for (int i = 0; longopts && longopts[i].name; i++) {
        if ((strncmp(longopts[i].name, arg, len) == 0) && (strlen(longopts[i].name) == len)) {
            match = i;
            break;
        }
    }
    if (match < 0) {
        if (opterr) {
            fprintf(stderr, "%s: unrecognized option '--%.*s'\n", argv[0], (int)len, arg);
        }
        optopt = 0;
        return '?';
    }
    const struct option *o = &longopts[match];
    optarg = NULL;
    if (o->has_arg == no_argument) {
        if (eq) {
            if (opterr) {
                fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n", argv[0], o->name);
            }
            optopt = o->val;
            return '?';
        }
    } else if (eq) {
        optarg = (char *)eq + 1;
    } else if (o->has_arg == required_argument) {
        if (optind < argc) {                /* as GNU getopt: the next element, even if it starts with "-" */
            take_separate_argument(argc, argv);
        } else {
            if (opterr) {
                fprintf(stderr, "%s: option '--%s' requires an argument\n", argv[0], o->name);
            }
            optopt = o->val;
            return '?';
        }
    }
    if (longindex) {
        *longindex = match;
    }
    if (o->flag) {
        *o->flag = o->val;
        return 0;
    }
    return o->val;
}

static int parse_short(int argc, char **argv, const char *optstring)
{
    char *arg = argv[optind];
    char c;
    const char *spec;

    if (next_char == 0) {
        next_char = 1;
    }
    c = arg[next_char++];
    spec = (c != ':') ? strchr(optstring, c) : NULL;
    optarg = NULL;

    if (spec == NULL) {
        if (opterr) {
            fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
        }
        optopt = c;
        if (arg[next_char] == '\0') {
            optind++;
            next_char = 0;
        }
        return '?';
    }
    if (spec[1] == ':') {
        if (arg[next_char] != '\0') {            /* "-k100" */
            optarg = &arg[next_char];
            optind++;
        } else if (spec[2] == ':') {             /* optional argument: only in the same element */
            optind++;
        } else {                                  /* "-k 100" */
            optind++;
            if ((optind < argc) && (strcmp(argv[optind], "--") != 0)) {
                take_separate_argument(argc, argv);
            } else {
                if (opterr) {
                    fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);
                }
                optopt = c;
                next_char = 0;
                return (optstring[0] == ':') ? ':' : '?';
            }
        }
        next_char = 0;
    } else if (arg[next_char] == '\0') {
        optind++;
        next_char = 0;
    }
    return c;
}

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex)
{
    char **av = (char **)argv;   /* permutation reorders argv, as GNU getopt does */

    optarg = NULL;
    if (optind == 0) {           /* optind = 0 restarts the scan (GNU convention) */
        optind = 1;
        next_char = 0;
    }
    if (next_char == 0) {
        if ((optind >= argc) || !find_next_option(argc, av, optstring, longopts)) {
            return -1;
        }
        if ((av[optind][1] == '-') && (av[optind][2] != '\0')) {
            return parse_long(argc, av, longopts, longindex);
        }
    }
    return parse_short(argc, av, optstring);
}

int getopt(int argc, char *const argv[], const char *optstring)
{
    return getopt_long(argc, argv, optstring, NULL, NULL);
}
