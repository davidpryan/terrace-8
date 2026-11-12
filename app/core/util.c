/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Ryan */
/* core/util.c: small text helpers shared by LEDGER-4, MYCEL-8 and SPOOL-4 (the device has no string.h). */

static uint32_t s_len(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}
static void s_cpy(char *d, const char *s)
{
    while ((*d++ = *s++))
        ;
}
static void s_cat(char *d, const char *s) { s_cpy(d + s_len(d), s); }

static void i_text(int32_t v, char *b)          /* signed, no padding */
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}
static void i_cat(char *d, int32_t v) { i_text(v, d + s_len(d)); }
static void sgn_cat(char *d, int32_t v)          /* +3, -2, 0 */
{
    if (v > 0)
        s_cat(d, "+");
    i_cat(d, v);
}
static void hex2(uint32_t v, char *b)            /* two hex digits */
{
    static const char H[] = "0123456789ABCDEF";
    b[0] = H[(v >> 4) & 15u];
    b[1] = H[v & 15u];
    b[2] = 0;
}
static void pad2(int32_t v, char *b)             /* two decimal digits, 00..99 */
{
    b[0] = (char)('0' + (v / 10) % 10);
    b[1] = (char)('0' + v % 10);
    b[2] = 0;
}

static const char *const NOTE_NM[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
static void note_text(int32_t n, char *b)        /* tracker style: C-4, F#3 */
{
    b[0] = NOTE_NM[n % 12][0];
    b[1] = NOTE_NM[n % 12][1];
    b[2] = (char)('0' + (n / 12 - 1 < 0 ? 0 : n / 12 - 1));
    b[3] = 0;
}
static void note_name(int32_t n, char *b)        /* C4, F#3 */
{
    uint32_t k = 0;
    b[k++] = NOTE_NM[n % 12][0];
    if (NOTE_NM[n % 12][1] == '#')
        b[k++] = '#';
    b[k] = 0;
    i_cat(b, n / 12 - 1);
}

/* ---- scales: masks of the 12 semitones (bit 0 the root) ---- */
enum { SC_CHROM, SC_MAJOR, SC_MINOR, SC_DORIAN, SC_PHRYG, SC_LYDIAN, SC_MIXO, SC_PENTA, SC_MPENTA, SC_HARM, SC_BLUES,
       SC_WHOLE, NSCALE };
static const uint16_t SCALE_MASK[NSCALE] = {0xFFF, 0xAB5, 0x5AD, 0x6AD, 0x5AB, 0xAD5, 0x6B5, 0x295, 0x4A9, 0x9AD,
                                            0x4E9, 0x555};
static const char *const SCALE_NM[NSCALE] = {"CHROM", "MAJOR", "MINOR", "DORIAN", "PHRYG", "LYDIAN", "MIXO",
                                             "PENTA", "M.PENT", "HARM", "BLUES", "WHOLE"};
static const char *const ROOT_NM[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static uint32_t scale_n(uint32_t sc)             /* notes in the scale */
{
    uint32_t m = SCALE_MASK[sc % NSCALE], n = 0;
    while (m) {
        n += m & 1u;
        m >>= 1;
    }
    return n;
}
/* degree d (any integer) of the scale from root (0..11), as a MIDI note from base (the root's octave) */
static int32_t scale_deg(int32_t d, uint32_t root, uint32_t sc, int32_t base)
{
    int32_t n = (int32_t)scale_n(sc), oct = d >= 0 ? d / n : -((-d + n - 1) / n), i = d - oct * n, k;
    uint32_t m = SCALE_MASK[sc % NSCALE];
    for (k = 0; k < 12; k++)
        if (m >> k & 1u) {
            if (i-- == 0)
                break;
        }
    return base + (int32_t)root + oct * 12 + k;
}
static int32_t scale_snap(int32_t note, uint32_t root, uint32_t sc)   /* the nearest note in the scale, down first */
{
    uint32_t m = SCALE_MASK[sc % NSCALE];
    int32_t d;
    for (d = 0; d < 12; d++) {
        if (m >> (uint32_t)(((note - d - (int32_t)root) % 12 + 12) % 12) & 1u)
            return note - d;
        if (m >> (uint32_t)(((note + d - (int32_t)root) % 12 + 12) % 12) & 1u)
            return note + d;
    }
    return note;
}

/* a small deterministic generator: the same seed, the same music */
static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}
