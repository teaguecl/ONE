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
// Game Genie codes remembered per game (cheats.h; ctest cheats_per_game). Runs in a directory of its own with
// XDG_CONFIG_HOME there, so no real cheats file is touched.
//   cheats_test NESTEST_ROM
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cheats.h"
#include "mapper.h"
#include "sha1.h"

static int failures = 0;

static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

// the same path, '\\' and '/' alike (Windows paths are made with backslashes)
static int path_is(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        if (*a != *b && !((*a == '/' || *a == '\\') && (*b == '/' || *b == '\\'))) {
            return 0;
        }
    }
    return *a == *b;
}

static int file_has(const char *path, const char *text)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return strstr(buf, text) != NULL;
}

int main(int argc, char **argv)
{
    // from a clean start (a previous run's files would be read back)
    remove("config/one/cheats/Game A [11111111].cht");
    remove("config/one/cheats/Game B [22222222].cht");
    remove("config/one/cheats/Game C [33333333].cht");

    // the game's identity: the No-Intro CRC-32 of PRG + CHR without the header (python3 zlib.crc32(rom[16:]))
    if (argc > 1) {
        check("nestest loads", mapper_init(argv[1]));
        check("its No-Intro CRC-32 is 158B0388", mapper_rom_crc() == 0x158B0388u);
        mapper_deinit();
    }

    // SHA-1: the FIPS 180 test vector, and nestest's data (python3 hashlib.sha1(rom[16:]))
    sha1_ctx sha;
    char hex[41];
    sha1_init(&sha);
    sha1_update(&sha, (const uint8_t *)"abc", 3);
    sha1_hex(&sha, hex);
    check("SHA-1 of \"abc\"", strcmp(hex, "A9993E364706816ABA3E25717850C26C9CD0D89D") == 0);
    if (argc > 1) {
        mapper_init(argv[1]);
        check("nestest's data SHA-1", strcmp(mapper_rom_sha1(), "4131307F0F69F2A5C54B7D438328C5B2A5ED0820") == 0);
        mapper_deinit();
    }

    // the code databases, by SHA-1 (no ROM needed): the built-in Ninja Gaiden (USA); a game only in the user's own
    // file (the test makes one in $XDG_CONFIG_HOME/one/cheats/); an unknown game has nothing
    cheats_set_game("games/ng.nes", 0x44444444u, "15F245161179AB1959B7DC20E82ADD024D23AA3D");
    check("database: built-in game found", strcmp(cheats_db_game(), "Ninja Gaiden (USA)") == 0);
    check("database: its cheats", cheats_db_count() == 4);
    char codes[128], desc[CHEATS_DESC_SIZE];
    check("database: entry 2 (several codes)", cheats_db_get(2, codes, sizeof(codes), desc, sizeof(desc)) &&
                                                   strcmp(codes, "XYETKSKN+XNXVESKN+XYKVKVKN") == 0 &&
                                                   strcmp(desc, "Ryu jumps higher") == 0);
    cheats_set_game("games/user.nes", 0x66666666u, "1111111111111111111111111111111111111111");
    check("database: the user's file", strcmp(cheats_db_game(), "Test Game (USA)") == 0 && cheats_db_count() == 2);
    check("database: user entry 0", cheats_db_get(0, codes, sizeof(codes), desc, sizeof(desc)) &&
                                        strcmp(codes, "SXIOPO") == 0 && strcmp(desc, "Test one") == 0);
    check("database: entry 0 into the list, ticked", cheats_db_add(0) == 1 && cheats_count() == 1 &&
                                                         cheats_get(0)->on && cheats_enabled() &&
                                                         strcmp(cheats_get(0)->description, "Test one") == 0);
    cheats_set_game("games/unknown.nes", 0x55555555u, "0000000000000000000000000000000000000000");
    check("database: an unknown game has no entries", cheats_db_game()[0] == '\0' && cheats_db_count() == 0);

    cheat_code a, b;
    cheats_decode("SXIOPO", &a);
    cheats_decode("AEUKZIGA", &b);

    // not remembered (headless runs, --batch, --cheat): no file
    cheats_set_game("games/Game A.nes", 0x11111111u, NULL);
    cheats_add(&a, "Infinite lives");
    check("not persistent: no file", cheats_file()[0] == '\0');

    // remembered: a game's list goes to config/one/cheats/<name> [<CRC>].cht at once
    cheats_set_persistent(1);
    cheats_set_game("games/Game B.nes", 0x22222222u, NULL);
    check("another game: an empty list", cheats_count() == 0);
    check("... with Game Genie off", !cheats_enabled() && !cheats_active);
    check("the file is named after the ROM and its CRC",
          path_is(cheats_file(), "config/one/cheats/Game B [22222222].cht"));
    check("ticking a code switches Game Genie on",
          cheats_add(&a, "Infinite lives") && cheats_enabled() && cheats_active);
    check("a second code, ticked", cheats_add(&b, "Start with 9 lives") && cheats_active_count() == 2);
    cheats_set_on(cheats_find(&b), 0);
    check("unticked: still in the list", cheats_count() == 2 && cheats_active_count() == 1);
    const char *path = "config/one/cheats/Game B [22222222].cht";
    check("written: gamegenie_enabled", file_has(path, "gamegenie_enabled = true"));
    check("written: the codes, descriptions and ticks",
          file_has(path, "cheat0_desc = \"Infinite lives\"\ncheat0_code = \"SXIOPO\"\ncheat0_enable = true") &&
              file_has(path, "cheat1_code = \"AEUKZIGA\"\ncheat1_enable = false"));

    // a power cycle of the same game keeps the list as it is
    cheats_set_game("games/Game B.nes", 0x22222222u, NULL);
    check("power cycle: the list stays", cheats_count() == 2 && cheats_enabled());

    // another game, then back: the list comes back from the file, found by the CRC (the ROM was renamed)
    cheats_set_game("games/Game C.nes", 0x33333333u, NULL);
    check("game C: empty, Game Genie off", cheats_count() == 0 && !cheats_enabled());
    cheats_set_game("elsewhere/renamed.nes", 0x22222222u, NULL);
    check("game B again (renamed): its file", path_is(cheats_file(), path));
    const cheat_entry *e0 = cheats_get(0), *e1 = cheats_get(1);
    check("... both codes back", cheats_count() == 2 && e0 != NULL && e1 != NULL);
    check("... with their descriptions and ticks", e0 != NULL && e1 != NULL && e0->on && !e1->on &&
                                                       strcmp(e0->description, "Infinite lives") == 0 &&
                                                       strcmp(e1->description, "Start with 9 lives") == 0);
    check("... and Game Genie on", cheats_enabled() && cheats_active);

    // Game Genie off is remembered too
    cheats_set_enabled(0);
    cheats_set_game("games/Game C.nes", 0x33333333u, NULL);
    cheats_set_game("games/Game B.nes", 0x22222222u, NULL);
    check("Game Genie off, remembered", !cheats_enabled() && !cheats_active && cheats_count() == 2);
    cheats_delete(0);
    cheats_set_game("games/Game C.nes", 0x33333333u, NULL);
    cheats_set_game("games/Game B.nes", 0x22222222u, NULL);
    check("a deleted code stays deleted", cheats_count() == 1 && cheats_find(&a) < 0);

    printf("%s\n", failures == 0 ? "cheats_test: all passed" : "cheats_test: FAILED");
    return failures == 0 ? 0 : 1;
}
