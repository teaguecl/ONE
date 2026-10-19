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
// Game Genie codes: a code replaces what the CPU bus reads from one ROM address
// ($8000-$FFFF); an 8-letter code only when the ROM byte there is its compare value (bank-switched games). Applied
// in the mapper's read path (mapper.c puts read_prg_with_cheats() in front of the mapper's read_prg while a code is
// on: no cost otherwise), so DMA reads and the debugger see them too. Not machine state.
//
// Each game has a list of codes, each ticked (on) or not, and Game Genie on or off for the game. In interactive use
// (one with a window, one-debugger's UI: cheats_set_persistent()) the list is remembered per game, in
// $XDG_CONFIG_HOME/one/cheats/<ROM name> [<CRC>].cht (RetroArch's .cht format, plus gamegenie_enabled), found by
// the game's No-Intro CRC-32 so that a renamed ROM keeps its codes.
#ifndef cheats_h_defined
#define cheats_h_defined
#include <stdbool.h>
#include <stdint.h>

#define CHEATS_MAX            64 // codes in a game's list at most
#define CHEATS_GAME_GENIE_MAX 3  // what the real Game Genie takes (more work, with a warning)
#define CHEATS_DESC_SIZE      96 // a description, with its terminating zero

typedef struct {
    uint16_t address; // $8000-$FFFF
    uint8_t value;    // what the CPU reads there instead
    bool has_compare; // 8-letter code:
    uint8_t compare;  // ... only when the ROM byte there is this
} cheat_code;

typedef struct {
    cheat_code code;
    char description[CHEATS_DESC_SIZE]; // "" = none
    bool on;                            // ticked: applied (while Game Genie is on)
} cheat_entry;

// "SXIOPO", "sxi-opo", "AEUKZIGA": 6 or 8 of the letters A P Z L G I T Y E O X U K S V N ('-' and ' ' ignored)
bool cheats_decode(const char *text, cheat_code *code);
// The standard spelling (the third letter's top bit set in 8-letter codes): TEXT gets 7 or 9 bytes
void cheats_encode(const cheat_code *code, char text[9]);

// The game's list
int cheats_count(void);
const cheat_entry *cheats_get(int index);
int cheats_find(const cheat_code *code); // its index, or -1
// Put CODE in the list (or find it there) and tick it, which also switches Game Genie on for the game.
// DESCRIPTION: NULL or "" keeps the one it has. False: the list is full, or not a ROM address.
bool cheats_add(const cheat_code *code, const char *description);
void cheats_set_on(int index, bool on); // tick / untick (ticking switches Game Genie on)
void cheats_delete(int index);
void cheats_clear(void); // delete them all
// Game Genie on/off for the game (the codes stay listed, ticked or not)
void cheats_set_enabled(bool on);
bool cheats_enabled(void);
int cheats_active_count(void); // ticked codes (applied while Game Genie is on)

// Remember the list per game (one with a window, one-debugger's UI); off: nothing is read or written
void cheats_set_persistent(bool on);
// emu_power_on(): the game, by its No-Intro CRC-32 and SHA-1 (of PRG + CHR without the header; SHA1: 40 hex digits,
// or NULL). Another game: its own list (remembered, or empty with Game Genie off); a power cycle of the same game
// keeps the list.
void cheats_set_game(const char *rom_filename, uint32_t crc, const char *sha1);
const char *cheats_file(void); // the game's file ("" when nothing is remembered)

// The known codes for the game, found by the game's SHA-1. Two sources, the user's first: nes_gamegenie.txt in the
// "cheats" folder of ONE's config directory (~/.config/one/cheats/; ONE ships none: tools/cheats/build_db.py makes
// it from libretro-database), then the few codes built in (data/cheats/default.txt). An entry is one cheat: one
// code or several ("XTUTILSV+XTUTTUGE") that work together.
const char *cheats_db_game(void); // the game's No-Intro name ("" when the database does not know it)
int cheats_db_count(void);        // its known cheats
bool cheats_db_get(int index, char *codes, int codes_size, char *description, int description_size);
int cheats_db_add(int index); // put entry INDEX's codes in the list, ticked: how many went in (0: none)

// The read path: codes on at all (a single test while none are), and the patch itself
extern bool cheats_active;
uint8_t cheats_patch(uint16_t address, uint8_t rom_value);

struct state_io;
void cheats_state(struct state_io *io); // (nothing is saved: codes are not machine state)
#endif
