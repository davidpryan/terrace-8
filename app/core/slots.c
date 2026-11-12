/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Ryan */
/* core/slots.c: four project slots in the FM-1's data area, whole sectors each. A project is one block of bytes
 * (the app's struct), written after a 16-byte header that goes last, so a save cut short never loads.
 * Only a sector's first 0xF00 bytes are used: the update loader looks for its record at 0xF00 of sectors in the
 * extra windows (fm1-base plat.h), so those bytes stay erased. The settings and the platform's records live in the
 * store objects at 0xDC000 and above (app.h APP_STORE_MAP), outside these runs. */

#define NSLOTS 4u
#define SECT_USE 0xF00u
#ifndef SLOT_SECTS
#define SLOT_SECTS 16u
#endif
#ifdef APP_SLOT_BASES                                   /* the app's own places for the four (each SLOT_SECTS sectors) */
static const uint32_t SLOT_BASES[NSLOTS] = APP_SLOT_BASES;
#define SLOT_SECT_ADDR(slot, sect) (SLOT_BASES[(slot) % NSLOTS] + (sect) * 0x1000u)
#else
#define SLOT_BASE 0x98000u                              /* 0x98000 .. 0xD8000: 64 sectors, inside PLAT_DATA */
#define SLOT_SECT_ADDR(slot, sect) (SLOT_BASE + ((slot) * SLOT_SECTS + (sect)) * 0x1000u)
#endif
#define SLOT_BYTES (SLOT_SECTS * SECT_USE)              /* 61,440 B with 16 sectors */

typedef struct {
    uint32_t magic, len, crc, ver;
} slot_hdr_t;

static uint32_t crc32_step(uint32_t c, const uint8_t *p, uint32_t n)
{
    uint32_t i, k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8u; k++)
            c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}

static uint32_t slot_at(uint32_t slot, uint32_t off)
{
    return SLOT_SECT_ADDR(slot, off / SECT_USE) + off % SECT_USE;
}

static void slot_read(uint32_t slot, uint32_t off, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    while (n) {
        uint32_t room = SECT_USE - off % SECT_USE, k = n < room ? n : room, i;
        const uint8_t *src = plat_data(slot_at(slot, off));
        for (i = 0; i < k; i++)
            d[i] = src ? src[i] : 0xFFu;
        d += k;
        off += k;
        n -= k;
    }
}

/* 0 saved; -1 too big; -2 the flash failed */
static int slot_save(uint32_t slot, uint32_t magic, uint32_t ver, const void *src, uint32_t len)
{
    const uint8_t *s = src;
    uint32_t off = 0, sect;
    slot_hdr_t h;
    if (slot >= NSLOTS || len + sizeof h > SLOT_BYTES)
        return -1;
    for (sect = 0; sect * SECT_USE < len + sizeof h; sect++)
        if (plat_data_erase(SLOT_SECT_ADDR(slot, sect)))
            return -2;
    while (off < len) {                                 /* the body after the header's place */
        uint32_t at = off + sizeof h, room = SECT_USE - at % SECT_USE, k = len - off < room ? len - off : room;
        if (k > 256u)
            k = 256u;
        if (plat_data_write(slot_at(slot, at), s + off, k))
            return -2;
        off += k;
    }
    h.magic = magic;
    h.len = len;
    h.crc = ~crc32_step(0xFFFFFFFFu, s, len);
    h.ver = ver;
    return plat_data_write(slot_at(slot, 0), &h, sizeof h) ? -2 : 0;
}

/* the slot's length if it holds a good project of this magic, else 0 */
static uint32_t slot_used(uint32_t slot, uint32_t magic)
{
    slot_hdr_t h;
    if (slot >= NSLOTS)
        return 0;
    slot_read(slot, 0, &h, sizeof h);
    if (h.magic != magic || h.len == 0u || h.len + sizeof h > SLOT_BYTES)
        return 0;
    return h.len;
}

/* 1 loaded (exactly len bytes, CRC good); 0 not: dst left as it was */
static int slot_load(uint32_t slot, uint32_t magic, void *dst, uint32_t len)
{
    slot_hdr_t h;
    uint8_t chunk[64];
    uint32_t off, crc = 0xFFFFFFFFu;
    if (slot_used(slot, magic) != len)
        return 0;
    slot_read(slot, 0, &h, sizeof h);
    for (off = 0; off < len; off += sizeof chunk) {
        uint32_t k = len - off < sizeof chunk ? len - off : sizeof chunk;
        slot_read(slot, sizeof h + off, chunk, k);
        crc = crc32_step(crc, chunk, k);
    }
    if (~crc != h.crc)
        return 0;
    slot_read(slot, sizeof h, dst, len);
    return 1;
}

static void slot_clear(uint32_t slot)
{
    static const uint8_t z[4] = {0, 0, 0, 0};
    if (slot < NSLOTS)
        plat_data_write(slot_at(slot, 0), z, sizeof z);
}
