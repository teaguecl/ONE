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
// Reading a game out of a .zip file (zipfile.h). Own small inflate (after zlib's contrib/puff, public domain): no
// library, and every read is bounds checked because the file is not to be trusted.
#include "zipfile.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#define strcasecmp  _stricmp /* MSVC spellings of the POSIX functions */
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif
#include "crc32.h"

// --- inflate ---------------------------------------------------------------------------------------------------

#define MAXBITS   15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct {
    const uint8_t *in;
    size_t in_size, in_pos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t out_size, out_pos;
} inflate_state;

typedef struct {
    short count[MAXBITS + 1];
    short symbol[FIXLCODES];
} huffman;

// NEED bits (0..16), least significant first; -1: out of input
static int get_bits(inflate_state *s, int need)
{
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_size) {
            return -1;
        }
        val |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

// The symbol of the next code; -1: out of input, -2: not a code
static int decode(inflate_state *s, const huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int bit = get_bits(s, 1);
        if (bit < 0) {
            return -1;
        }
        code |= bit;
        int count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -2;
}

// Builds the decoding tables from N code lengths. 0: complete code, >0: incomplete, <0: over-subscribed
static int construct(huffman *h, const short *length, int n)
{
    short offs[MAXBITS + 1];
    for (int len = 0; len <= MAXBITS; len++) {
        h->count[len] = 0;
    }
    for (int symbol = 0; symbol < n; symbol++) {
        h->count[length[symbol]]++;
    }
    if (h->count[0] == n) {
        return 0;
    }
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) {
            return left;
        }
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) {
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    }
    for (int symbol = 0; symbol < n; symbol++) {
        if (length[symbol] != 0) {
            h->symbol[offs[length[symbol]]++] = (short)symbol;
        }
    }
    return left;
}

static bool inflate_codes(inflate_state *s, const huffman *lencode, const huffman *distcode)
{
    static const short lens[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dists[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int symbol = decode(s, lencode);
        if (symbol < 0) {
            return false;
        }
        if (symbol < 256) {
            if (s->out_pos >= s->out_size) {
                return false;
            }
            s->out[s->out_pos++] = (uint8_t)symbol;
        } else if (symbol == 256) {
            return true;
        } else {
            symbol -= 257;
            if (symbol >= 29) {
                return false;
            }
            int extra = get_bits(s, lext[symbol]);
            if (extra < 0) {
                return false;
            }
            size_t len = (size_t)lens[symbol] + (size_t)extra;
            int dsym = decode(s, distcode);
            if (dsym < 0 || dsym >= 30) {
                return false;
            }
            extra = get_bits(s, dext[dsym]);
            if (extra < 0) {
                return false;
            }
            size_t dist = (size_t)dists[dsym] + (size_t)extra;
            if (dist > s->out_pos || len > s->out_size - s->out_pos) {
                return false;
            }
            for (size_t i = 0; i < len; i++) { // (may overlap: byte by byte)
                s->out[s->out_pos + i] = s->out[s->out_pos + i - dist];
            }
            s->out_pos += len;
        }
    }
}

static bool inflate_stored(inflate_state *s)
{
    s->bitbuf = 0; // discard the rest of the byte
    s->bitcnt = 0;
    if (s->in_size - s->in_pos < 4) {
        return false;
    }
    unsigned len = s->in[s->in_pos] | (unsigned)s->in[s->in_pos + 1] << 8;
    unsigned nlen = s->in[s->in_pos + 2] | (unsigned)s->in[s->in_pos + 3] << 8;
    s->in_pos += 4;
    if (len != (~nlen & 0xFFFF) || len > s->in_size - s->in_pos || len > s->out_size - s->out_pos) {
        return false;
    }
    memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->in_pos += len;
    s->out_pos += len;
    return true;
}

static bool inflate_fixed(inflate_state *s)
{
    huffman lencode, distcode;
    short lengths[FIXLCODES];
    int symbol = 0;
    for (; symbol < 144; symbol++) {
        lengths[symbol] = 8;
    }
    for (; symbol < 256; symbol++) {
        lengths[symbol] = 9;
    }
    for (; symbol < 280; symbol++) {
        lengths[symbol] = 7;
    }
    for (; symbol < FIXLCODES; symbol++) {
        lengths[symbol] = 8;
    }
    construct(&lencode, lengths, FIXLCODES);
    for (symbol = 0; symbol < MAXDCODES; symbol++) {
        lengths[symbol] = 5;
    }
    construct(&distcode, lengths, MAXDCODES);
    return inflate_codes(s, &lencode, &distcode);
}

static bool inflate_dynamic(inflate_state *s)
{
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[MAXLCODES + MAXDCODES];
    huffman lencode, distcode;
    int nlen = get_bits(s, 5), ndist = get_bits(s, 5), ncode = get_bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) {
        return false;
    }
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) {
        return false;
    }
    int index = 0;
    for (; index < ncode; index++) {
        int v = get_bits(s, 3);
        if (v < 0) {
            return false;
        }
        lengths[order[index]] = (short)v;
    }
    for (; index < 19; index++) {
        lengths[order[index]] = 0;
    }
    if (construct(&lencode, lengths, 19) != 0) { // must be complete
        return false;
    }
    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, &lencode);
        if (symbol < 0) {
            return false;
        }
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
        } else {
            int len = 0, repeat, base;
            if (symbol == 16) {
                if (index == 0) {
                    return false;
                }
                len = lengths[index - 1];
                repeat = get_bits(s, 2);
                base = 3;
            } else if (symbol == 17) {
                repeat = get_bits(s, 3);
                base = 3;
            } else {
                repeat = get_bits(s, 7);
                base = 11;
            }
            if (repeat < 0 || index + base + repeat > nlen + ndist) {
                return false;
            }
            for (repeat += base; repeat > 0; repeat--) {
                lengths[index++] = (short)len;
            }
        }
    }
    if (lengths[256] == 0) { // no end-of-block code
        return false;
    }
    int err = construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) {
        return false;
    }
    err = construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) {
        return false;
    }
    return inflate_codes(s, &lencode, &distcode);
}

// Inflates the raw deflate stream IN into OUT, which must come out full to the last byte
static bool inflate_exact(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size)
{
    inflate_state s = {in, in_size, 0, 0, 0, out, out_size, 0};
    int last;
    do {
        last = get_bits(&s, 1);
        int type = get_bits(&s, 2);
        if (last < 0 || type < 0) {
            return false;
        }
        bool ok = type == 0   ? inflate_stored(&s)
                  : type == 1 ? inflate_fixed(&s)
                  : type == 2 ? inflate_dynamic(&s)
                              : false;
        if (!ok) {
            return false;
        }
    } while (!last);
    return s.out_pos == out_size;
}

// --- the ZIP container -----------------------------------------------------------------------------------------

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t rd32(const uint8_t *p)
{
    return rd16(p) | rd16(p + 2) << 16;
}

static void set_error(char *error, size_t error_size, const char *text)
{
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "%s", text);
    }
}

bool zip_is_zip_name(const char *name)
{
    size_t len = strlen(name);
    return len >= 4 && strcasecmp(name + len - 4, ".zip") == 0;
}

// A name worth taking: ends in .nes, not a directory, not the "._name" / __MACOSX debris of a Mac
static bool wanted_entry(const char *name, size_t len)
{
    if (len < 5 || strncasecmp(name + len - 4, ".nes", 4) != 0) {
        return false;
    }
    if (len >= 9 && strncasecmp(name, "__MACOSX/", 9) == 0) {
        return false;
    }
    size_t base = len;
    while (base > 0 && name[base - 1] != '/' && name[base - 1] != '\\') {
        base--;
    }
    return !(len - base >= 2 && name[base] == '.' && name[base + 1] == '_');
}

bool zip_extract_nes_memory(const uint8_t *memory, size_t size, uint8_t **data, size_t *data_size, char *entry_name,
                            size_t entry_name_size, char *error, size_t error_size)
{
    *data = NULL;
    *data_size = 0;
    if (size < 22) {
        set_error(error, error_size, "not a ZIP file (too short)");
        return false;
    }
    // end of central directory: the last record with the signature (it may be followed by a comment)
    size_t eocd = size - 22;
    size_t scan_floor = size > 22 + 65535 ? size - 22 - 65535 : 0;
    for (;; eocd--) {
        if (rd32(memory + eocd) == 0x06054b50u) {
            break;
        }
        if (eocd == scan_floor) {
            set_error(error, error_size, "not a ZIP file (no central directory)");
            return false;
        }
    }
    const uint8_t *e = memory + eocd;
    uint32_t entries = rd16(e + 10), cd_size = rd32(e + 12), cd_off = rd32(e + 16);
    if (rd16(e + 4) != 0 || rd16(e + 6) != 0 || rd16(e + 8) != entries) {
        set_error(error, error_size, "multi-part ZIP files are not supported");
        return false;
    }
    if (entries == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {
        set_error(error, error_size, "ZIP64 files are not supported");
        return false;
    }
    if (cd_off > eocd || cd_size > eocd - cd_off) {
        set_error(error, error_size, "damaged ZIP file (central directory out of bounds)");
        return false;
    }
    const uint8_t *cd = memory + cd_off;
    size_t pos = 0;
    for (uint32_t i = 0; i < entries; i++) {
        if (cd_size - pos < 46 || rd32(cd + pos) != 0x02014b50u) {
            set_error(error, error_size, "damaged ZIP file (bad central directory entry)");
            return false;
        }
        const uint8_t *h = cd + pos;
        uint32_t flags = rd16(h + 8), method = rd16(h + 10), crc = rd32(h + 16), csize = rd32(h + 20);
        uint32_t usize = rd32(h + 24), name_len = rd16(h + 28), extra_len = rd16(h + 30), comment_len = rd16(h + 32);
        uint32_t local_off = rd32(h + 42);
        size_t record = 46 + (size_t)name_len + extra_len + comment_len;
        if (record > cd_size - pos) {
            set_error(error, error_size, "damaged ZIP file (bad central directory entry)");
            return false;
        }
        const char *name = (const char *)h + 46;
        pos += record;
        if (!wanted_entry(name, name_len)) {
            continue;
        }
        if (flags & 1) {
            set_error(error, error_size, "the ROM in the ZIP is encrypted");
            return false;
        }
        if (method != 0 && method != 8) {
            set_error(error, error_size, "the ROM in the ZIP uses a compression method other than deflate");
            return false;
        }
        if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || local_off == 0xFFFFFFFFu) {
            set_error(error, error_size, "ZIP64 files are not supported");
            return false;
        }
        if (usize > ZIP_MAX_ROM_SIZE || usize < 16) {
            set_error(error, error_size, "the file in the ZIP is not the size of a NES ROM");
            return false;
        }
        if (method == 0 && csize != usize) {
            set_error(error, error_size, "damaged ZIP file (sizes of a stored entry differ)");
            return false;
        }
        if (local_off > size || size - local_off < 30 || rd32(memory + local_off) != 0x04034b50u) {
            set_error(error, error_size, "damaged ZIP file (bad local header)");
            return false;
        }
        size_t data_off = (size_t)local_off + 30 + rd16(memory + local_off + 26) + rd16(memory + local_off + 28);
        if (data_off > size || csize > size - data_off) {
            set_error(error, error_size, "damaged ZIP file (entry out of bounds)");
            return false;
        }
        uint8_t *out = malloc(usize);
        if (out == NULL) {
            set_error(error, error_size, "out of memory");
            return false;
        }
        if (method == 0) {
            memcpy(out, memory + data_off, usize);
        } else if (!inflate_exact(memory + data_off, csize, out, usize)) {
            free(out);
            set_error(error, error_size, "damaged ZIP file (cannot inflate the ROM)");
            return false;
        }
        if ((crc32_update(0xFFFFFFFFu, out, usize) ^ 0xFFFFFFFFu) != crc) {
            free(out);
            set_error(error, error_size, "damaged ZIP file (CRC-32 of the ROM differs)");
            return false;
        }
        if (entry_name != NULL && entry_name_size > 0) {
            size_t base = name_len;
            while (base > 0 && name[base - 1] != '/' && name[base - 1] != '\\') {
                base--;
            }
            snprintf(entry_name, entry_name_size, "%.*s", (int)(name_len - base), name + base);
        }
        *data = out;
        *data_size = usize;
        return true;
    }
    set_error(error, error_size, "no .nes file in the ZIP");
    return false;
}

bool zip_extract_nes(const char *path, uint8_t **data, size_t *data_size, char *entry_name, size_t entry_name_size,
                     char *error, size_t error_size)
{
    *data = NULL;
    *data_size = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        set_error(error, error_size, "can't open the ZIP file");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || (unsigned long)size > ZIP_MAX_FILE_SIZE) {
        fclose(f);
        set_error(error, error_size, "the ZIP file is too large");
        return false;
    }
    uint8_t *memory = malloc((size_t)size + 1);
    if (memory == NULL || fread(memory, 1, (size_t)size, f) != (size_t)size) {
        free(memory);
        fclose(f);
        set_error(error, error_size, "can't read the ZIP file");
        return false;
    }
    fclose(f);
    bool ok =
        zip_extract_nes_memory(memory, (size_t)size, data, data_size, entry_name, entry_name_size, error, error_size);
    free(memory);
    return ok;
}
