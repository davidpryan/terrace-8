/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Ryan */
/* core/kit.c: the controls and the screen's frame, shared by the six firmwares.
 *   input    kit_input() once a frame: buttons and keys pressed / released since the last frame (edges), the encoders'
 *            turns, a button held alone for a second shows its help card (the app's CARD text) until it is let go.
 *   message  app_say(a, b): two words over the header for 1.3 s, and to a screen reader.
 *   lists    a page of rows (label, value): SELECT picks a row, PRESET (or K1) changes it, SEL acts on it.
 * The app defines: CARD_TEXT[NB] (a sentence per button, 0 for none), KIT_ACCENT (an RGB565 colour). */

#define K_BG RGB(10, 12, 14)
#define K_PANEL RGB(24, 28, 31)
#define K_LINE RGB(46, 52, 56)
#define K_FG RGB(232, 228, 216)
#define K_DIM RGB(118, 126, 124)
#define K_FAINT RGB(70, 76, 78)
#define K_REC RGB(240, 76, 70)
#define K_OK RGB(120, 226, 160)

static const char *const BTN_NM[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                       "PLAY", "REC", "OCT-", "OCT+"
#if PLAT_SMK37
                                       , "KBANK", "FBANK", "<", ">"
#endif
};
static const char *const ENC_NM[NE] = {"SELECT", "ALGO", "PRESETS", "K1", "K2", "K3", "K4"};
static const uint8_t WKEY[16] = PLAT_WHITE_KEYS, BKEY[11] = PLAT_BLACK_KEYS;

static struct {
    uint32_t btn, keys;                                 /* held now */
    uint32_t bdown, bup, kdown, kup;                    /* this frame's edges */
    int32_t enc[NE];                                    /* this frame's turns */
    uint32_t bt0[NB];                                   /* when each button went down */
    uint32_t chord;                                     /* buttons held that something else happened under */
    uint8_t card;                                       /* the button whose help card shows, NB none */
    uint8_t redraw;
    char msg[2][24];
    uint32_t msg_until;
    uint8_t vel[NKEYS];
} K;

static int white_of(uint32_t key)                       /* key -> white index 0..15, or -1 */
{
    uint32_t i;
    for (i = 0; i < 16u; i++)
        if (WKEY[i] == key)
            return (int)i;
    return -1;
}
static int black_of(uint32_t key)
{
    uint32_t i;
    for (i = 0; i < 11u; i++)
        if (BKEY[i] == key)
            return (int)i;
    return -1;
}

static void kit_input(void)
{
    uint32_t btn = plat_buttons(), keys, i, now = plat_ms();
    touch_poll();
    keys = touch_keys();
    K.bdown = btn & ~K.btn;
    K.bup = K.btn & ~btn;
    K.kdown = keys & ~K.keys;
    K.kup = K.keys & ~keys;
    for (i = 0; i < NKEYS; i++)
        if (K.kdown >> i & 1u)
            K.vel[i] = (uint8_t)clamp((int32_t)touch_vel(i), 1, 127);
    for (i = 0; i < NB; i++)
        if (K.bdown >> i & 1u)
            K.bt0[i] = now;
    for (i = 0; i < NE; i++) {
        K.enc[i] = plat_enc((int)i);
        if (K.enc[i])
            K.redraw = 1;
    }
    /* anything else happening while a button is held makes that button a modifier (no card, no tap) */
    if (K.kdown || K.bdown || K.enc[0] | K.enc[1] | K.enc[2] | K.enc[3] | K.enc[4] | K.enc[5] | K.enc[6])
        K.chord |= btn & ~K.bdown;
    K.chord &= btn | K.bup;
    if (K.bdown || K.bup || K.kdown || K.kup)
        K.redraw = 1;
    K.btn = btn;
    K.keys = keys;
    if (K.card < NB && !(btn >> K.card & 1u)) {
        K.card = NB;
        K.redraw = 1;
    }
}
/* after the app has read this frame: a button alone a second shows its card */
static void kit_cards(const char *const *text)
{
    uint32_t i, now = plat_ms();
    if (K.card < NB)
        return;
    for (i = 0; i < NB; i++)
        if ((K.btn >> i & 1u) && !(K.chord >> i & 1u) && now - K.bt0[i] >= 1000u && text[i] &&
            (K.btn & ~(1u << i)) == 0u && !K.keys) {
            K.card = (uint8_t)i;
            K.chord |= 1u << i;
            K.redraw = 1;
            if (ann_on())
                ann_say(text[i], 0, ANN_NOW);
        }
}
#define DOWN(b) (K.bdown >> (b) & 1u)
#define HELD(b) (K.btn >> (b) & 1u)
#define UP(b) (K.bup >> (b) & 1u)
#define TAPPED(b) (UP(b) && !(K.chord >> (b) & 1u) && plat_ms() - K.bt0[b] < 1000u)
static void kit_use(uint32_t b) { K.chord |= 1u << b; }   /* the held button acted as a modifier */

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[52];
    for (i = 0; i < 23u && a && a[i]; i++)
        K.msg[0][i] = a[i];
    K.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        K.msg[1][i] = b[i];
    K.msg[1][i] = 0;
    K.msg_until = plat_ms() + 1300u;
    K.redraw = 1;
    if (ann_on()) {
        s_cpy(t, K.msg[0]);
        s_cat(t, " ");
        s_cat(t, K.msg[1]);
        ann_say(t, 0, ANN_NOW);
    }
}

/* ---- drawing ---- */
static void box(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)   /* an outline */
{
    cv_rect(x, y, w, 1, c);
    cv_rect(x, y + h - 1, w, 1, c);
    cv_rect(x, y, 1, h, c);
    cv_rect(x + w - 1, y, 1, h, c);
}
static int32_t text_r(int32_t xr, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)   /* right-aligned */
{
    return cv_text(xr - text_w(f, s), y, f, s, c);
}
static void text_c(int32_t xc, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(xc - text_w(f, s) / 2, y, f, s, c);
}

/* the header band, 22 px: the name (or a message), then the page, then the transport at the right */
static void kit_header(const char *name, const char *page, uint16_t accent, int playing, int rec, uint32_t bpm10)
{
    char t[16];
    int32_t x;
    cv_begin(240, 22, K_BG);
    if (plat_ms() < K.msg_until) {
        x = cv_text(6, 3, &FONT_B, K.msg[0], K_FG);
        cv_text(x + 6, 3, &FONT_B, K.msg[1], accent);
    } else {
        x = cv_text(6, 3, &FONT_B, name, accent);
        cv_text(x + 8, 4, &FONT_XS, page, K_FG);
        t[0] = 0;
        i_cat(t, (int32_t)(bpm10 / 10u));
        s_cat(t, ".");
        i_cat(t, (int32_t)(bpm10 % 10u));
        x = 222 - text_w(&FONT_XS, t);
        cv_text(x, 4, &FONT_XS, t, K_DIM);
        if (rec)
            cv_rect(x - 12, 8, 7, 7, K_REC);
        if (playing) {                                  /* a play triangle */
            int32_t r;
            for (r = 0; r < 9; r++)
                cv_rect(226, 6 + r, (r < 5 ? r : 8 - r) + 1, 1, K_OK);
        }
    }
    cv_rect(0, 21, 240, 1, K_LINE);
    cv_blit(0, 0);
}

/* a help card over the page (the page's band, 240 x h at y): the button's name and its sentence, wrapped */
static void kit_card(uint32_t b, const char *text, int32_t h, uint16_t accent)
{
    char line[48];
    int32_t y = 30, x0 = 12;
    uint32_t i = 0, n;
    cv_rect(6, 6, 228, h - 12, K_PANEL);
    box(6, 6, 228, h - 12, accent);
    cv_text(x0, 10, &FONT_B, BTN_NM[b], accent);
    while (text[i] && y < h - 20) {                     /* words to a line while they fit */
        uint32_t last_space = 0, k = 0;
        n = 0;
        while (text[i + k] && k < 47u) {
            line[k] = text[i + k];
            line[k + 1] = 0;
            if (text_w(&FONT_XS, line) > 212)
                break;
            if (text[i + k] == ' ')
                last_space = k;
            k++;
        }
        n = (text[i + k] && last_space) ? last_space : k;
        line[n] = 0;
        cv_text(x0, y, &FONT_XS, line, K_FG);
        i += n;
        while (text[i] == ' ')
            i++;
        y += 15;
    }
}

/* ---- a list page: rows of a label and a value ---- */
typedef struct {
    const char *label;
    void (*value)(char *out);                           /* the value as text */
    void (*change)(int32_t d);                          /* PRESET / K1 turned (0: no change) */
    void (*act)(void);                                  /* SEL pressed on it (0: none) */
} row_def_t;

/* draw rows [first..] of a list from y, the cursor's row lit */
static void kit_list_draw(const row_def_t *rows, uint32_t n, uint32_t cur, int32_t y, int32_t h, uint16_t accent)
{
    uint32_t vis = (uint32_t)(h / 16), first = 0, i;
    char v[32];
    if (cur >= vis)
        first = cur - vis + 1u;
    for (i = first; i < n && i < first + vis; i++, y += 16) {
        int on = i == cur;
        if (on)
            cv_rect(2, y - 1, 236, 16, K_PANEL);
        cv_text(10, y, &FONT_XS, rows[i].label, on ? K_FG : K_DIM);
        v[0] = 0;
        if (rows[i].value)
            rows[i].value(v);
        else if (rows[i].act)
            s_cpy(v, "SEL >");
        text_r(230, y, &FONT_XS, v, on ? accent : K_FG);
    }
}
/* the controls of a list: SELECT the row, PRESET or K1 the value, SEL its action */
static void kit_list_input(const row_def_t *rows, uint32_t n, uint8_t *cur)
{
    int32_t d = K.enc[EN_PRESET] + K.enc[EN_K1];
    *cur = (uint8_t)clamp((int32_t)*cur + K.enc[EN_SELECT], 0, (int32_t)n - 1);
    if (d && rows[*cur].change)
        rows[*cur].change(d);
    if (DOWN(B_SEL) && rows[*cur].act)
        rows[*cur].act();
}

/* the bottom band, 18 px: what the four knobs do now */
static void kit_knobs(const char *a, const char *b, const char *c, const char *d, uint16_t accent)
{
    const char *k[4] = {a, b, c, d};
    uint32_t i;
    cv_begin(240, 18, K_BG);
    cv_rect(0, 0, 240, 1, K_LINE);
    for (i = 0; i < 4u; i++)
        if (k[i] && k[i][0]) {
            cv_rect(4 + (int32_t)i * 60, 7, 4, 4, accent);
            cv_text(11 + (int32_t)i * 60, 2, &FONT_T, k[i], K_DIM);
        }
    cv_blit(0, 222);
}

/* a value 0..max as a bar w wide (meters, levels) */
static void bar(int32_t x, int32_t y, int32_t w, int32_t h, int32_t v, int32_t max, uint16_t c)
{
    int32_t f = max > 0 ? v * w / max : 0;
    cv_rect(x, y, w, h, K_LINE);
    if (f > 0)
        cv_rect(x, y, f, h, c);
}
