/*
  SPDX-License-Identifier: GPL-2.0-or-later
  ONE NES emulator
  Copyright (C) 1999-2017 Chris Teague

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

#include <getopt.h> /* compat/getopt.h on toolchains without one (MSVC) */ // IWYU pragma: keep
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include "scheduler.h"
#include "libco/libco.h"
#include "cpu.h"
#include "trace.h"
#include "rp2a03.h"
#include "cheats.h"
#include "mapper.h"
#include "input.h"
#include "video.h"
#include "emu.h"
#include "audio.h"
#include "savestate.h"
#include "script.h"
#include "log.h"
#include "settings.h"
#include "frontend.h"
#include "version.h"
#ifdef ONE_HAVE_GUI
#include "gui/gui.h"
#endif

extern uint64_t glb_clock_stop; /* cpu.c */
static int dump_ram_start = -1; /* -D: CPU address range to print when emulation stops */
static int dump_ram_end = -1;
static bool headless = false;              /* -H: no window */
static bool cheat_given = false;           /* --cheat: only those codes this time */
static bool unthrottled = false;           /* -u: don't pace the window to 60.1 frames/s */
static const char *state_save_path = NULL; /* --state-save */
static uint32_t state_frame = 0;           /* --state-frame */
static const char *state_load_path = NULL; /* --state-load */
static const char *lua_paths[8];           /* --lua (in order) */
static int lua_count = 0;
static bool trace_on = false;             /* -t */
static const char *trace_path = NULL;     /* --trace FILE */
static uint32_t trace_sample = 0;         /* --trace-sample N */
static const char *log_level_spec = NULL; /* --log-level */

/* long-only options */
enum {
    OPT_CONFIG = 1000,
    OPT_WRITE_CONFIG,
    OPT_LIST_GAMEPADS,
    OPT_APU_LOG,
    OPT_WAV,
    OPT_STATE_SAVE,
    OPT_STATE_FRAME,
    OPT_STATE_LOAD,
    OPT_TRACE_FILE,
    OPT_TRACE_SAMPLE,
    OPT_LOG_FILE,
    OPT_LOG_LEVEL,
    OPT_LUA,
    OPT_INPUT,
    OPT_CHEAT,
    OPT_NO_SAV,
    OPT_SAV_IN
};
static int write_config(const char *path);
static int load_config(const char *path);

/* -t makes the trace stream fully buffered (the trace is millions of lines); flush it if we die on an assert or a
 * crash so the last trace lines before the failure are not lost. */
static char trace_buffer[1 << 16];
static void flush_trace_and_die(int sig)
{
    fflush(NULL); // NOLINT(bugprone-signal-handler): best effort; the process is dying anyway
    signal(sig, SIG_DFL);
    raise(sig);
}

/* static function declarations */
static void display_version(void);
static void display_usage(void);
static void display_help(void);
static int one_init(char *filename);
static bool start_trace(void);
static void dump_memory(void);

/*! \brief Main entrypoint of the emulator
 *  Detailed description starts here.
 * @param argc  argument count
 * @param argv pointer to array of argument strings
 * @return Nonzero on error.
 */
int main(int argc, char **argv)
{
    int status = -1; /* return status of this function */
    int option = 0;
    int option_index = 0;
    bool help_flag = false;    /* Indicates whether to display help */
    bool version_flag = false; /* Indicates whether to display version information */
    //bool debug_flag = false;
    //bool verbose_flag = false;
    //  char *cvalue = NULL;
    char *filename = NULL; /* points to the filename of the NES program to run */
    struct option long_options[] = {{"help", no_argument, NULL, (int)'h'},
                                    {"debug", no_argument, NULL, (int)'d'},
                                    {"verbose", no_argument, NULL, (int)'v'},
                                    {"version", no_argument, NULL, (int)'V'},
                                    {"create", required_argument, NULL, (int)'c'},
                                    {"clock", required_argument, NULL, (int)'k'},
                                    {"pc", required_argument, NULL, (int)'p'},
                                    {"regs", required_argument, NULL, (int)'R'},
                                    {"reset", required_argument, NULL, (int)'r'},
                                    {"start", required_argument, NULL, (int)'S'},
                                    {"dump", required_argument, NULL, (int)'D'},
                                    {"headless", no_argument, NULL, (int)'H'},
                                    {"trace", no_argument, NULL, (int)'t'},
                                    {"unthrottled", no_argument, NULL, (int)'u'},
                                    {"config", required_argument, NULL, OPT_CONFIG},
                                    {"write-config", required_argument, NULL, OPT_WRITE_CONFIG},
                                    {"list-gamepads", no_argument, NULL, OPT_LIST_GAMEPADS},
                                    {"apu-log", required_argument, NULL, OPT_APU_LOG},
                                    {"wav", required_argument, NULL, OPT_WAV},
                                    {"input", required_argument, NULL, OPT_INPUT},
                                    {"cheat", required_argument, NULL, OPT_CHEAT},
                                    {"no-sav", no_argument, NULL, OPT_NO_SAV},
                                    {"sav-in", required_argument, NULL, OPT_SAV_IN},
                                    {"state-save", required_argument, NULL, OPT_STATE_SAVE},
                                    {"state-frame", required_argument, NULL, OPT_STATE_FRAME},
                                    {"state-load", required_argument, NULL, OPT_STATE_LOAD},
                                    {"trace-file", required_argument, NULL, OPT_TRACE_FILE},
                                    {"trace-sample", required_argument, NULL, OPT_TRACE_SAMPLE},
                                    {"log", required_argument, NULL, OPT_LOG_FILE},
                                    {"log-level", required_argument, NULL, OPT_LOG_LEVEL},
                                    {"lua", required_argument, NULL, OPT_LUA},
                                    {NULL, 0, NULL, 0}};
    int start_pc = -1;
    int start_regs[5] = {-1, -1, -1, -1, -1}; /* a, x, y, s, p */
    const char *config_path = NULL;           /* --config */
    const char *write_config_path = NULL;     /* --write-config */
    bool list_gamepads = false;               /* --list-gamepads */
    const char *wav_path = NULL;              /* --wav */

    while ((option = getopt_long(argc, argv, "hdc:k:vVp:R:r:S:D:Htu", long_options, &option_index)) != -1) {
        switch (option) {
        case 0:
            /* nothing else to do */
            break;
        case 'h': help_flag = true; break;
        case 'd':
            //debug_flag = true;
            break;
        case 'c':
            //cvalue = optarg;
            break;
        case 'k': glb_clock_stop = strtoull(optarg, NULL, 10); break;
        case 'r': cpu_set_soft_reset_cycle(strtoull(optarg, NULL, 10)); break;
        case 'p': start_pc = (int)strtol(optarg, NULL, 16); break;
        case 'S': input_set_scripted_start(strtoull(optarg, NULL, 10)); break;
        case 'D':
            if ((2 != sscanf(optarg, "%x-%x", (unsigned *)&dump_ram_start, (unsigned *)&dump_ram_end)) ||
                (dump_ram_start > dump_ram_end) || (dump_ram_end > 0xFFFF)) {
                LOG_ERROR(EMU, "-D expects a hex address range: START-END (e.g. 0300-04FF)");
                return -1;
            }
            break;
        case 'R':
            if (5 != sscanf(optarg, "%x,%x,%x,%x,%x", (unsigned *)&start_regs[0], (unsigned *)&start_regs[1],
                            (unsigned *)&start_regs[2], (unsigned *)&start_regs[3], (unsigned *)&start_regs[4])) {
                LOG_ERROR(EMU, "-R expects five hex values: a,x,y,s,p");
                return -1;
            }
            break;
        case 'v': log_level_spec = "debug"; break;
        case 'V': version_flag = true; break;
        case 'H': headless = true; break;
        case 't': trace_on = true; break;
        case 'u': unthrottled = true; break;
        case OPT_CONFIG: config_path = optarg; break;
        case OPT_WRITE_CONFIG: write_config_path = optarg; break;
        case OPT_LIST_GAMEPADS: list_gamepads = true; break;
        case OPT_APU_LOG: {
            FILE *log = fopen(optarg, "w");
            if (log == NULL) {
                LOG_ERROR(EMU, "cannot write the APU log %s", optarg);
                return -1;
            }
            rp2a03_set_log(log); /* closed at exit (the C library flushes it) */
            break;
        }
        case OPT_WAV: wav_path = optarg; break;
        case OPT_INPUT:
            if (!input_load_script(optarg)) {
                return 1;
            }
            break;
        case OPT_CHEAT: {
            cheat_code code;
            cheat_given = true;
            if (!cheats_decode(optarg, &code) || !cheats_add(&code, NULL)) {
                LOG_ERROR(EMU, "--cheat expects a Game Genie code (6 or 8 of the letters APZLGITYEOXUKSVN)");
                return 1;
            }
            break;
        }
        case OPT_NO_SAV: mapper_set_sav_files(false); break;
        case OPT_SAV_IN: mapper_set_sav_input(optarg); break;
        case OPT_STATE_SAVE: state_save_path = optarg; break;
        case OPT_STATE_FRAME: state_frame = (uint32_t)strtoul(optarg, NULL, 10); break;
        case OPT_STATE_LOAD: state_load_path = optarg; break;
        case OPT_TRACE_FILE:
            trace_on = true;
            trace_path = optarg;
            break;
        case OPT_TRACE_SAMPLE:
            trace_on = true;
            trace_sample = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case OPT_LOG_FILE:
            if (!log_set_file_path(optarg)) {
                LOG_ERROR(EMU, "cannot write the log file %s", optarg);
                return -1;
            }
            break;
        case OPT_LUA:
            if (lua_count == (int)(sizeof(lua_paths) / sizeof(lua_paths[0]))) {
                LOG_ERROR(EMU, "too many --lua scripts");
                return -1;
            }
            lua_paths[lua_count++] = optarg;
            break;
        case OPT_LOG_LEVEL: {
            char error[160];
            if (!log_set_levels(optarg, error, sizeof(error))) { // (checked now, applied after the settings file)
                LOG_ERROR(EMU, "--log-level: %s", error);
                return -1;
            }
            log_level_spec = optarg;
            break;
        }
        default: display_usage(); return -1;
        }
    }

    /* display the help screen if arguments say to, then exit */
    if (help_flag == true) {
        display_help();
        return (0);
    }
    /* display the version information if arguments say to, then exit */
    if (version_flag == true) {
        display_version();
        return (0);
    }

    if (write_config_path != NULL) {
        return write_config(write_config_path);
    }
    if (list_gamepads) {
        return input_list_gamepads();
    }

    /* filename must be the first non-option argument, so read it in */
    filename = argv[optind];
#ifdef ONE_HAVE_GUI
    gui_install(); /* the menu of the game window (Esc) */
#endif
    if (filename == NULL && (headless || !fe_menu_installed())) {
        /* no file name given and no menu to choose one with: show usage and exit with error status */
        display_usage();
        return (1);
    }

    /* check if more than one non-option argument was passed.  This is not allowed */
    if ((argc - optind) > 1) {
        LOG_ERROR(EMU, "unknown command line argument '%s'", argv[optind + 1]);
        display_usage();
        return (1);
    }

    //  if ( SDL_Init(SDL_INIT_AUDIO|SDL_INIT_VIDEO) < 0 ) {
    //    exit(1);
    //  }
    //  atexit(SDL_Quit);

    if (trace_on && !start_trace()) { // (first: from now on the messages keep out of a trace on stderr)
        return 2;
    }
    if (load_config(config_path) != 0) {
        return 2;
    }
    if (log_level_spec != NULL) { // the command line wins over the settings file
        char error[160];
        log_set_levels(log_level_spec, error, sizeof(error));
    }
    if (wav_path != NULL && !audio_set_wav(wav_path)) {
        LOG_ERROR(EMU, "cannot write '%s'", wav_path);
        return 2;
    }
    cpu_set_start_state(start_pc, start_regs[0], start_regs[1], start_regs[2], start_regs[3], start_regs[4]);

    status = one_init(filename);
    return (status);
}

/*******************************************************************************
 * FUNCTION NAME : display_help()
 * ARGUMENTS     : void
 * RETURN        : void
 * DESCRIPTION   : This function displays the help information for the program.
 *                 This function will be called if the '-h' or '--help' options
 *                 are passed on the command line.
 *******************************************************************************/
static void display_help(void)
{
    printf("Usage: one [OPTIONS] [FILE]\n");
    printf(
        "Example: one game.nes   (or game.zip: its first .nes file; without FILE the menu opens, to choose a game)\n");
    printf("\n");
    printf("Options:\n");
    printf("  -v, --verbose             more messages: the same as --log-level debug\n");
    printf("      --log-level SPEC      how much each part tells: off, error, warn, info (default), debug or trace,\n");
    printf("                            for all (debug) and/or by part (ppu=trace,apu=debug); also log.* in one.cfg\n");
    printf("      --log FILE            write messages to FILE instead of stderr\n");
    printf("  -k, --clock N             stop after CPU cycle N (0 or absent: run until the window is closed)\n");
    printf("  -H, --headless            no window (tests, traces); builds without SDL2 are always headless\n");
    printf("  -t, --trace               write a Mesen-format trace line per instruction to stderr (slow);\n");
    printf("                            messages then go to stdout, so that stderr has only the trace\n");
    printf("      --trace-file FILE     write the trace to FILE instead (implies -t)\n");
    printf("      --trace-sample N      -t, but with PPU catch-up on: a line at about every N-th instruction only\n");
    printf("  -u, --unthrottled         run as fast as possible instead of at NES speed (60.1 frames/s)\n");
    printf("  -p, --pc HEX              start execution at PC=HEX after reset (e.g. -p C000 for nestest)\n");
    printf("  -R, --regs A,X,Y,S,P      set registers (hex) after reset\n");
    printf("  -r, --reset N             press RESET at the first instruction boundary at/after CPU cycle N;\n");
    printf("                            -k then counts cycles after the reset\n");
    printf("  -S, --start N             hold START on controller 1 for 2 frames from CPU cycle N\n");
    printf("      --input FILE          press the buttons FILE lists by CPU cycle ('CYCLE BUTTONS [BUTTONS2]',\n");
    printf("                            e.g. '742270 start'; regression/input_from_log.py makes it from a Mesen log;\n"
           "                            'CYCLE zapper X Y [fire]' or 'CYCLE zapper off [fire]' aims the Zapper)\n");
    printf("      --cheat CODE          a Game Genie code (6 or 8 letters, e.g. SXIOPO); may be repeated\n");
    printf("  -D, --dump START-END      when emulation stops, print CPU memory START-END (hex) to stdout\n");
    printf("                            as lines of 'RAM xxxx: hh hh ...' (16 bytes per line)\n");
    printf(
        "      --config FILE         settings (default ./one.cfg, else ~/.config/one/one.cfg): controllers, sound\n");
    printf("      --write-config FILE   write a settings file with every setting at its default, then exit\n");
    printf("      --list-gamepads       list the connected gamepads and show the names of their buttons\n");
    printf("      --wav FILE            also write the sound to FILE (16-bit mono WAV), also when headless\n");
    printf(
        "      --no-sav              neither read nor write the .sav file of a game with battery RAM (traces start from empty RAM)\n");
    printf(
        "      --sav-in FILE         start the battery RAM from FILE and write no .sav (a trace that began from a save)\n");
    printf("      --state-save FILE     save the state to FILE at the end of the frame --state-frame N (default 0)\n");
    printf("      --state-load FILE     start from the state in FILE (F1/F2 in the window: slots next to the ROM)\n");
    printf("      --lua FILE            run a Lua script (Mesen 2 API: emu.addEventCallback, emu.read...);\n");
    printf("                            up to 8, in order\n");
    printf("      --apu-log FILE        write the APU state at the end of every frame, in the format of\n");
    printf("                            tools/mesen/apu_log.lua (compare with tools/apu_compare.py)\n");
    printf("  -V, --version             print version information and exit\n");
    printf("\n");
    printf("In the window: Esc opens the menu (games, save states, settings); F1/F2 save/load a state.\n");
    printf("      --help                display this help and exit\n");
    printf("\n");
    printf("\nProject home is <http://>\n");
}

/*******************************************************************************
 * FUNCTION NAME : display_version()
 * ARGUMENTS     : void
 * RETURN        : void
 * DESCRIPTION   : This function simply displays the version information for the
 *                 program.  This will be called if the '--version' option is
 *                 passed on the command line.
 *******************************************************************************/
static void display_version(void)
{
    printf("ONE (ONE NES Emulator) %s\n", one_version());
    printf("\n");
    printf("Copyright 1999-2026 Chris Teague\n");
    printf(
        "This is free software; see the source for copying conditions. There is NO warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n");
}

/*******************************************************************************
 * FUNCTION NAME : display_usage()
 * ARGUMENTS     : void
 * RETURN        : void
 * DESCRIPTION   : This function displays the usage information for the program.
 *                 This function will be called if incorrect parameters are
 *                 passed on the command line.
 *******************************************************************************/
static void display_usage(void)
{
    fprintf(stderr, "Usage: one [OPTIONS] FILE\n");
    fprintf(stderr, "Try `one --help' for more information.\n");
}


/* -D: side-effect-free peek, so dumping PPU/APU registers doesn't change anything (program output: stdout) */
static void dump_memory(void)
{
    for (int line = dump_ram_start & ~0x0F; line <= dump_ram_end; line += 16) {
        printf("RAM %04X:", line);
        for (int a = line; a < line + 16; a++) {
            if ((a < dump_ram_start) || (a > dump_ram_end)) {
                printf("   ");
            } else {
                printf(" %02X", cpu_peek((uint16_t)a));
            }
        }
        printf("\n");
    }
}

/* -t / --trace-file FILE: the trace has a stream of its own. On stderr (no FILE), log messages move to stdout meanwhile. */
static bool start_trace(void)
{
    FILE *out = stderr;
    if (trace_path != NULL) {
        out = fopen(trace_path, "w");
        if (out == NULL) {
            LOG_ERROR(EMU, "cannot write the trace file %s", trace_path);
            return false;
        }
    } else {
        log_set_trace_on_stderr(true);
    }
    setvbuf(out, trace_buffer, _IOFBF, sizeof(trace_buffer));
    trace_set_file(out);
    cpu_set_trace(true);
    const char *seed = getenv("ONE_TRACE_SEED");
    cpu_set_trace_sample(trace_sample, seed != NULL ? (uint32_t)strtoul(seed, NULL, 10) : 1);
    signal(SIGABRT, flush_trace_and_die);
    signal(SIGSEGV, flush_trace_and_die);
    return true;
}

static int one_init(char *filename)
{
    video_init(headless); /* falls back to headless if no window can be opened */
    // Game Genie codes are remembered per game when a person plays (not headless runs, tests, traces); --cheat codes
    // are for this run only
    cheats_set_persistent(!video_is_headless() && !cheat_given);
    video_set_throttle(!unthrottled);
    input_init(); /* gamepads (with a window only) */
    audio_init(video_is_headless());

    char rom[1024];
    int status_no_rom = 0;
    if (filename == NULL) {
        /* no ROM on the command line: the menu chooses one */
        if (!fe_menu_available()) {
            LOG_ERROR(EMU, "no window for the menu: give the ROM on the command line");
            status_no_rom = 1;
        } else if (fe_run_menu(false) == FE_OPEN_ROM) {
            snprintf(rom, sizeof(rom), "%s", fe_next_rom());
            filename = rom;
        }
    }
    bool first = true;
    int status = status_no_rom;
    while (filename != NULL) {
        LOG_INFO(EMU, "Emulation started on file %s", filename);
        if (true != emu_power_on(filename)) {
            status = -1;
            break;
        }
        if (first && state_load_path != NULL && !savestate_load_file(state_load_path)) {
            status = -1;
            break;
        }
        if (first && state_save_path != NULL) {
            savestate_save_file_at_frame(state_save_path, state_frame);
        }
        if (first) {
            for (int i = 0; i < lua_count; i++) {
                if (!script_load(lua_paths[i])) {
                    status = -1; // (the error is in the log)
                }
            }
            if (status == -1) {
                break;
            }
        }
        first = false;
        fe_set_next_rom(NULL);
        sched_enter(cpu_get_thread());

        if (dump_ram_start >= 0) {
            dump_memory();
        }
        /* the menu ended the game to start one: another, or the same again (power cycle) */
        if (fe_next_rom()[0] == '\0') {
            break;
        }
        snprintf(rom, sizeof(rom), "%s", fe_next_rom());
        filename = rom;
        emu_power_off();
    }

    LOG_DEBUG(EMU, "shutting down");
    script_stop_all();
    if (script_exit_code() != 0) {
        status = script_exit_code(); // emu.stop(code)
    }
    emu_power_off();
    audio_shutdown();
    input_shutdown();
    video_shutdown();

    return status;
}

/* The settings file (one.cfg): --config FILE, else the default places. Returns nonzero if FILE was given but
 * cannot be read. Mistakes in the file are reported but do not stop the emulator. */
static int load_config(const char *path)
{
    return settings_load(path) < 0 ? 1 : 0;
}

static int write_config(const char *path)
{
    FILE *out = fopen(path, "w");
    if (out == NULL) {
        LOG_ERROR(CFG, "cannot write '%s'", path);
        return 1;
    }
    settings_write_defaults(out);
    fclose(out);
    LOG_INFO(CFG, "wrote %s", path);
    return 0;
}
