/*
  SPDX-License-Identifier: GPL-2.0-or-later
  ONE NES emulator
  Copyright (C) 1999-2009 Chris Teague

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

#ifndef ines_h_defined
#define ines_h_defined

#include <stddef.h>
/* http://wiki.nesdev.com/w/index.php/INES */
#include <stdint.h>
#include <stdbool.h>

typedef struct INES_header {
    char identification[4]; /* MUST be "INES\0x1A" */
    uint8_t prg_16kb_banks;
    uint8_t chr_8kb_banks;
    uint8_t flags_6;
    uint8_t flags_7;
    uint8_t prg_ram_8kb_banks;
    uint8_t flags_9;
    uint8_t flags_10;
    uint8_t zero_filled[5];
} ines_header;

typedef enum INES_mapper {
    NROM,
    SxROM,
    UxROM,
    CNROM,       // 3
    TxROM,       // 4 (MMC3)
    ExROM,       // 5 (MMC5)
    AxROM = 7,   // 7
    PxROM = 9,   // 9 (MMC2)
    FxROM = 10,  // 10 (MMC4)
    RAMBO1 = 64, // 64 (Tengen RAMBO-1)
    ColorDreams = 11,
    Namco163 = 19, // 19
    VRC6a = 24,    // 24
    VRC6b = 26,    // 26
    VRC4ac = 21,   // 21 (VRC4a, VRC4c)
    VRC2a = 22,    // 22
    VRC2b = 23,    // 23 (VRC2b, VRC4e, VRC4f)
    VRC4bd = 25,   // 25 (VRC4b, VRC4d, VRC2c)
    BNROM = 34,
    NESEvent = 105, // 105 (NES-EVENT, Nintendo World Championships 1990)
    GxROM = 66,
    BandaiFCG = 16, // 16 (Bandai FCG, LZ93D50 + 24C02)
    FME7 = 69,      // 69 (Sunsoft FME-7, 5B)
    Camerica = 71,
    IremHolyDiver = 78,
    NINA003 = 79,
    Bandai74161 = 70, // 70
    VRC1 = 75,        // 75 (Konami VRC1)
    JalecoJF13 = 86,  // 86
    JalecoJF05 = 87,  // 87
    NINA006 = 113,    // 113 (NINA-03 / NINA-06 multicarts)
    Sunsoft1 = 184,   // 184
    CNROM185 = 185,   // 185 (CNROM with copy protection)
    IremG101 = 32,    // 32
    TaitoTC0190 = 33, // 33
    Quattro = 232,    // 232 (Camerica Quattro)
    Sachen243 = 243,  // 243 (Sachen 74LS374N)
    Namco108 = 206,   // 206 (DxROM: Namco 108 / MIMIC-1)
    TxSROM = 118,
    TQROM = 119,
    UNKNOWN_MAPPER,
} ines_mapper;

/* this structure holds all of the information read from a INES file */
typedef struct INES_data {
    ines_header header;
    uint8_t *trainer;
    uint8_t *prg_rom;
    uint8_t *chr_rom;
} ines_data;


bool ines_load(char *filename, ines_data *);
bool ines_load_memory(const uint8_t *data, size_t size, const char *name, ines_data *);
void ines_free_data(ines_data *);
uint8_t ines_get_mirroring(ines_data *ines_file_data);


#endif
