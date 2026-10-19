/*
  SPDX-License-Identifier: GPL-2.0-or-later
  ONE NES emulator
  Copyright (C) 1999-2005 Chris Teague

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

#ifndef mapper_h_defined
#define mapper_h_defined

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ines.h"

struct state_io;

// battery-backed PRG-RAM contents (.sav file)
typedef struct {
    size_t size;
    unsigned char *data;
} sav_data_t;

// NES 2.0 header fields a mapper may need. ines.c does not decode NES 2.0: a mapper that supports it calls
// mapper_nes2_info() (the others treat the file as iNES 1.0).
typedef struct {
    unsigned submapper;      // byte 8, high nibble
    uint32_t prg_ram_size;   // byte 10 low nibble: PRG-RAM in bytes (0 = none)
    uint32_t prg_nvram_size; // byte 10 high nibble: battery-backed PRG-RAM in bytes (0 = none)
    uint32_t chr_ram_size;   // byte 11 low nibble: CHR-RAM in bytes (0 = none)
} nes2_info_t;
bool mapper_is_nes2(const ines_header *h);
// Decodes H into INFO. False (with the reason logged) if the file needs what ONE cannot do: a mapper number above
// 255 (mapper.c only reads bytes 6 and 7) or ROM sizes in byte 9 (ines.c only reads bytes 4 and 5).
bool mapper_nes2_info(const ines_header *h, nes2_info_t *info);

// Who is reading the PPU bus (mapper_info_t::ppu_fetch): the picture fetches of the rendering PPU, or the CPU through $2007
// (PPU_FETCH_IDLE: the unused name table fetches at the end of a line, 257-320 and 337-340)
typedef enum { PPU_FETCH_BG, PPU_FETCH_SPRITE, PPU_FETCH_CPU, PPU_FETCH_IDLE } ppu_fetch_kind;

// all the operations a mapper must implement
typedef struct {
    void (*deinit)(void);
    uint8_t (*read_prg)(uint16_t);
    void (*write_prg)(uint16_t, uint8_t);
    uint8_t (*read_chr)(uint16_t);
    void (*write_chr)(uint16_t, uint8_t);
    uint16_t (*get_ciram_addr)(uint16_t);
    uint8_t (*debug_read_chr)(uint16_t);
    void (*get_sav_data)(sav_data_t *sav);
    uint8_t *(*debug_prg_ram)(size_t *size);        // debugger: the cartridge's PRG-RAM at $6000 (NULL if none)
    int32_t (*debug_prg_offset)(uint16_t address);  // debugger: PRG-ROM offset mapped at $8000+ (NULL: not banked)
    int32_t (*debug_chr_offset)(uint16_t address);  // debugger: CHR offset mapped at $0000-$1FFF (NULL: unknown)
    const uint8_t *(*debug_chr_data)(size_t *size); // debugger: the CHR-ROM or CHR-RAM those offsets are into
    void (*state)(struct state_io *io);             // save states: the mapper's registers and RAM (savestate.h)
    void (*ppu_a12)(bool high, uint64_t dot);       // PPU address line A12 changed (NULL: the mapper ignores it)
    // PPU catch-up (FEATURE_PPU_CATCHUP): the fewest counted A12 rises after which the mapper may request an IRQ now, or
    // UINT32_MAX if none can (IRQ off, or already requested). NULL with ppu_a12 set: "1" (catch-up only far from the picture)
    uint32_t (*a12_rises_to_irq)(void);
    // PPU catch-up with a mapper that watches the PPU bus (ppu_fetch): the fewest scanline starts (dot 1 of a picture line or of the
    // pre-render line, while rendering) after which the mapper may request an IRQ, 0 if one may come now, UINT32_MAX if none can
    uint32_t (*lines_to_irq)(void);
    // Expansion hardware (MMC5): CPU reads of $4020-$5FFF (writes arrive in write_prg). False: nothing drives the bus (open bus).
    bool (*read_exp)(uint16_t address, uint8_t *value);
    // Peek of the same addresses for the debugger and traces: no side effects (a read of $5204 acknowledges the IRQ).
    // False: the address high byte, like unmapped memory
    bool (*debug_read_exp)(uint16_t address, uint8_t *value);
    // The PPU reads ADDRESS ($0000-$2FFF) on its bus, DOT = time in PPU dots. Called before the read, for every fetch of
    // the picture and for the CPU's $2007 reads (NULL: the mapper does not look; then neither this nor ppu_ctrl_write cost anything)
    void (*ppu_fetch)(uint16_t address, ppu_fetch_kind kind, uint64_t dot);
    void (*cpu_tick)(
        void); // once per CPU cycle, at the end of it (expansion audio: NULL for the others, who then cost nothing)
    void (*ppu_ctrl_write)(uint8_t value); // the CPU wrote $2000 (the sprite size matters to the MMC5)
    // Name table data that is not in the PPU's CIRAM: get_ciram_addr() returns a value >= $800 for such a name table byte, and
    // the PPU reads / writes it with these (ADDRESS is the PPU address, $2000-$2FFF)
    uint8_t (*read_nt)(uint16_t address);
    void (*write_nt)(uint16_t address, uint8_t value);
    // Debugger: write a byte of the cartridge's RAM as it is mapped at ADDRESS now ($6000-$DFFF), whatever the write protection says;
    // false if there is none there (NULL: the debugger writes PRG-RAM $6000-$7FFF as one block, debug_prg_ram())
    bool (*debug_write_prg)(uint16_t address, uint8_t value);
    // Debugger's background view: the tile whose name table byte is at NT_ADDRESS ($2000-$2FFF) gets its pattern from the 4 KB CHR bank
    // at *CHR_OFFSET and the palette *PALETTE, not the picture's (MMC5 extended attributes); false: no such override
    bool (*debug_bg_tile)(uint16_t nt_address, int32_t *chr_offset, int *palette);
    // CHR-ROM switched off (mapper 185 copy protection): nothing drives the PPU data bus at ADDRESS, and a $2007 read's buffer
    // gets the low byte of the address the PPU puts on the bus after the read (NULL: always driven)
    bool (*chr_is_open)(void);
    uint8_t (*debug_read_prg)(uint16_t address); // debugger/trace peek of $6000-$FFFF (NULL: same as read_prg)
    uint32_t prg_bank_size; // debugger: the mapper's PRG bank size in bytes (0: no PRG banking): "bank 5" in views
    ines_header header;     // copy of the ROM's iNES header, set by mapper_init()
    char *sav_filename;     // .sav path for the loaded ROM (malloc'd), freed by mapper_deinit()
} mapper_info_t;

bool mapper_init(char *filename);
// A game already in memory (NAME is for messages). The libretro core: the frontend hands the game over, and keeps the
// battery RAM itself (mapper_set_sav_files(false): no .sav file is read or written; mapper_battery_ram(): where it is)
bool mapper_init_memory(const uint8_t *data, size_t size, const char *name);
void mapper_set_sav_files(bool on);          // default on
void mapper_set_sav_input(const char *path); // battery RAM starts from this file; no .sav is read or written (--sav-in)
bool mapper_battery_ram(uint8_t **data, size_t *size); // false: no battery-backed RAM
void mapper_deinit(void);
uint8_t mapper_read_prg(uint16_t address);
void mapper_write_prg(uint16_t address, uint8_t value);
uint8_t mapper_read_chr(uint16_t address);
bool mapper_chr_is_open(void); // see mapper_info_t::chr_is_open
void mapper_write_chr(uint16_t address, uint8_t value);
uint16_t mapper_get_ciram_addr(uint16_t addr);
void mapper_ppu_a12(bool high, uint64_t dot); // the PPU drove A12 to a new level; dot = time in PPU dots
uint32_t mapper_a12_rises_to_irq(void);       // (see mapper_info_t::a12_rises_to_irq)
uint32_t mapper_lines_to_irq(void);           // (see mapper_info_t::lines_to_irq)
extern bool mapper_watches_ppu; // the mapper has a ppu_fetch hook (MMC5): the PPU calls it, and runs without catch-up
bool mapper_read_exp(uint16_t address, uint8_t *value); // CPU read of $4020-$5FFF; false: open bus
bool mapper_debug_read_exp(uint16_t address, uint8_t *value);
extern bool mapper_ticks; // the mapper has a cpu_tick hook
void mapper_cpu_tick(void);
bool mapper_debug_write_prg(uint16_t address, uint8_t value);
bool mapper_debug_bg_tile(uint16_t nt_address, int32_t *chr_offset, int *palette);
extern bool mapper_has_exp; // the mapper has a read_exp hook
void mapper_ppu_fetch(uint16_t address, ppu_fetch_kind kind, uint64_t dot);
void mapper_ppu_ctrl_write(uint8_t value);
uint8_t mapper_read_nt(uint16_t address);
void mapper_write_nt(uint16_t address, uint8_t value);
extern bool mapper_watches_a12;                      // the mapper has a ppu_a12 hook (else the PPU skips A12 tracking)
uint8_t mapper_debug_read_prg(uint16_t address);     // as the CPU reads it (Game Genie codes included)
uint8_t mapper_debug_read_prg_rom(uint16_t address); // the cartridge's own byte (without codes)
uint32_t mapper_rom_crc(void);                       // the loaded game's No-Intro CRC-32 (PRG + CHR without the header)
const char *mapper_rom_sha1(void); // ... and its SHA-1 (40 hex digits): the Game Genie code database's key
void mapper_update_cheats(void);   // Game Genie codes went on or off (cheats.c): patch the reads, or stop


uint8_t mapper_debug_read_chr(uint16_t address);
// Debugger: the cartridge's PRG-RAM ($6000-$7FFF) as stored by the mapper, regardless of enable bits; NULL if none
uint8_t *mapper_debug_prg_ram(size_t *size);
// Debugger: the PRG-ROM offset (from the start of PRG-ROM, iNES header excluded) that the CPU sees at an address
// $8000-$FFFF right now, or -1 (not PRG-ROM / no ROM loaded)
int32_t mapper_debug_prg_offset(uint16_t address);
const uint8_t *mapper_debug_prg_rom(size_t *size); // the whole PRG-ROM (NULL if none); scripts
const uint8_t *mapper_debug_chr_rom(size_t *size); // the whole CHR-ROM (NULL if the cartridge has CHR-RAM)
// Debugger: where the pattern byte at a PPU address $0000-$1FFF comes from right now: its offset in the cartridge's
// CHR-ROM or CHR-RAM (mapper_debug_chr_data()), or -1 if the mapper does not say. The PPU records it for each picture
// line (ppu_debug_line_scroll()), so that the background viewer draws a line with the CHR banks it was drawn with.
int32_t mapper_debug_chr_offset(uint16_t address);
const uint8_t *mapper_debug_chr_data(size_t *size); // the CHR-ROM or CHR-RAM as the mapper holds it (NULL: none)
// Debugger: the mapper switches PRG-ROM banks (a CPU address in $8000-$FFFF can show different ROM bytes)
bool mapper_debug_prg_banked(void);
// The PRG bank an address of $8000-$FFFF shows now, in the mapper's own bank size (MMC1 16KB, MMC3 8KB...), and
// the number of such banks; false when the mapper does not bank PRG (or for other addresses)
bool mapper_debug_prg_bank(uint16_t address, int *bank, int *banks);
// Debugger: the cartridge has battery-backed PRG-RAM (iNES flags 6 bit 1)
bool mapper_debug_has_battery(void);

#endif
