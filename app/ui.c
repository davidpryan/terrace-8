/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Ryan */
/* TERRACE-8's controls and screen.
 *   white keys 1-8    line A's stages      white keys 9-16   line B's stages
 *   a stage's key held: K1 pitch, K2 pulses, K3 gate mode, K4 ratchets; black keys 1-5 its gate mode,
 *                       6 slide, 7 skip, 8 accent
 *   a stage's key tapped: it is the stage SELECT (pitch) and PRESETS (pulses) change
 *   nothing held: K1 direction, K2 stages in play, K3 pulse length, K4 accumulator, of the line shown (ALGO)
 *   REC: the keys write pitches into the stages one after another       ARP: dice the line's pitches (SEQ: undo)
 *   FX: re-sync      LFO: mute the line      EDIT: the line's SOUND      GLO: setup      OCT: the line's octave
 *   SEL held + PRESETS: the scale; SEL held + SELECT: the root */

#define ACCENT RGB(240, 170, 90)
enum { PG_LINES, PG_SOUND, PG_SETUP };
static const uint16_t LINE_C[NLINE] = {RGB(240, 170, 90), RGB(110, 200, 230)};

#define SET_MAGIC 0x31524554u
typedef struct {
    uint32_t magic;
    uint8_t slot, ble_off, rsv[2];
} settings_t;
static settings_t set;

static struct {
    uint8_t page, line, stage, srow, grow;
    int8_t held;                                        /* the stage key held: line*8+stage, -1 none */
    uint8_t rec, rec_at;
    uint8_t dirty, save_wait;
    line_t undo;
    uint8_t undo_ok;
    int16_t key_note[NKEYS];
} U;

static const char *const CARD_TEXT[NB] = {
    "FX: RE-SYNC. Both lines start again from their first stage, the accumulators at zero.",
    "SEL held: PRESETS changes the scale, SELECT the root. Stages keep degrees, so the lines follow.",
    0,
    "LFO mutes or unmutes the line shown.",
    "EDIT: the line's SOUND (wave, filter, envelope, glide, MIDI channel).",
    "GLO: SETUP. Tempo, key, clock, echo and the four project slots.",
    "HOME: the two lines.",
    "SAVE writes the project to its slot. While playing it saves at the stop.",
    "ARP: DICE. New pitches for the line shown, from the scale. SEQ takes it back.",
    "SEQ: UNDO the last dice.",
    "PLAY starts and stops.",
    ("REC: the keys write pitches into the line's stages, one after another from the stage shown. "
     "REC again to stop. MIDI in writes too."),
    "OCT-: the line an octave down.",
    "OCT+: the line an octave up.",
#if PLAT_SMK37
    0, 0, 0, 0,
#endif
};

static void dirty(void) { U.dirty = 1; }
static line_t *ln(void) { return &P.line[U.line % NLINE]; }
static stage_t *stg(void) { return &P.line[U.line % NLINE].s[U.stage % NSTAGE]; }
static void settings_save(void) { plat_store_save(OBJ_SETTINGS, &set, sizeof set); }
int terrace_ble_off(void) { return set.ble_off; }

static void project_save_now(void)
{
    int r;
    P.bpm10 = T.bpm10;
    r = slot_save(set.slot, PROJ_MAGIC, PROJ_VER, &P, sizeof P);
    if (r == 0)
        U.dirty = 0;
    app_say(r == 0 ? "SAVED" : "SAVE", r == 0 ? "" : "FAILED");
}
static void transport_wait_stop(void)
{
    uint32_t t0 = plat_ms();
    T.want = 2;
    while (T.playing && plat_ms() - t0 < 200u)
        plat_yield();
}

/* ---- SOUND and SETUP rows ---- */
#define CUR_INS (&P.ins[U.line % NLINE])
static void v_u8(char *o, uint32_t v) { i_text((int32_t)v, o); }
static void ic(uint8_t *f, int32_t d, int32_t lo, int32_t hi) { *f = (uint8_t)clamp((int32_t)*f + d, lo, hi); dirty(); }
static void sv_wave(char *o) { s_cpy(o, WAVE_NM[CUR_INS->wave % NWAVE]); }
static void sv_cut(char *o) { v_u8(o, CUR_INS->cutoff); }
static void sv_res(char *o) { v_u8(o, CUR_INS->reso); }
static void sv_env(char *o) { v_u8(o, CUR_INS->envmod); }
static void sv_att(char *o) { v_u8(o, CUR_INS->attack); }
static void sv_dec(char *o) { v_u8(o, CUR_INS->decay); }
static void sv_sus(char *o) { v_u8(o, CUR_INS->sustain); }
static void sv_rel(char *o) { v_u8(o, CUR_INS->release); }
static void sv_gl(char *o) { v_u8(o, CUR_INS->glide); }
static void sv_lvl(char *o) { v_u8(o, CUR_INS->level); }
static void sv_echo(char *o) { v_u8(o, CUR_INS->echo); }
static void sv_gate(char *o) { v_u8(o, ln()->gate_len); s_cat(o, "%"); }
static void sv_chan(char *o) { if (CUR_INS->chan) v_u8(o, CUR_INS->chan); else s_cpy(o, "OFF"); }
static void sc_wave(int32_t d) { ic(&CUR_INS->wave, d, 0, NWAVE - 1); }
static void sc_cut(int32_t d) { ic(&CUR_INS->cutoff, d, 0, 127); }
static void sc_res(int32_t d) { ic(&CUR_INS->reso, d, 0, 127); }
static void sc_env(int32_t d) { ic(&CUR_INS->envmod, d, 0, 127); }
static void sc_att(int32_t d) { ic(&CUR_INS->attack, d, 0, 127); }
static void sc_dec(int32_t d) { ic(&CUR_INS->decay, d, 0, 127); }
static void sc_sus(int32_t d) { ic(&CUR_INS->sustain, d, 0, 127); }
static void sc_rel(int32_t d) { ic(&CUR_INS->release, d, 0, 127); }
static void sc_gl(int32_t d) { ic(&CUR_INS->glide, d, 0, 127); }
static void sc_lvl(int32_t d) { ic(&CUR_INS->level, d, 0, 127); }
static void sc_echo(int32_t d) { ic(&CUR_INS->echo, d, 0, 127); }
static void sc_gate(int32_t d) { ic(&ln()->gate_len, d * 5, 5, 100); }
static void sc_chan(int32_t d) { ic(&CUR_INS->chan, d, 0, 16); }
static const row_def_t SOUND_ROWS[] = {
    {"WAVE", sv_wave, sc_wave, 0},    {"CUTOFF", sv_cut, sc_cut, 0},      {"RESONANCE", sv_res, sc_res, 0},
    {"ENV > FILTER", sv_env, sc_env, 0}, {"ATTACK", sv_att, sc_att, 0},   {"DECAY", sv_dec, sc_dec, 0},
    {"SUSTAIN", sv_sus, sc_sus, 0},   {"RELEASE", sv_rel, sc_rel, 0},     {"SLIDE TIME", sv_gl, sc_gl, 0},
    {"GATE LENGTH", sv_gate, sc_gate, 0}, {"LEVEL", sv_lvl, sc_lvl, 0},   {"ECHO SEND", sv_echo, sc_echo, 0},
    {"MIDI CHANNEL", sv_chan, sc_chan, 0},
};
#define NSOUND_ROWS (sizeof SOUND_ROWS / sizeof SOUND_ROWS[0])

static void gv_tempo(char *o) { o[0] = 0; i_cat(o, T.bpm10 / 10); s_cat(o, "."); i_cat(o, T.bpm10 % 10); }
static void gv_root(char *o) { s_cpy(o, ROOT_NM[P.root % 12u]); }
static void gv_scale(char *o) { s_cpy(o, SCALE_NM[P.scale % NSCALE]); }
static void gv_clk(char *o) { s_cpy(o, CK_NM[T.clk_mode % NCK]); }
static void gv_clko(char *o) { s_cpy(o, T.clk_send ? "SEND" : "OFF"); }
static void gv_et(char *o) { v_u8(o, P.echo_time); }
static void gv_ef(char *o) { v_u8(o, P.echo_fb); }
static void gv_slot(char *o) { o[0] = 0; i_cat(o, set.slot + 1); s_cat(o, slot_used(set.slot, PROJ_MAGIC) ? "  (SAVED)" : "  (EMPTY)"); }
static void gv_ble(char *o) { s_cpy(o, set.ble_off ? "OFF" : "ON"); }
static void gc_tempo(int32_t d) { T.bpm10 = (uint16_t)clamp((int32_t)T.bpm10 + d * 10, 300, 3000); P.bpm10 = T.bpm10; dirty(); }
static void gc_root(int32_t d) { P.root = (uint8_t)((P.root + 12 + d % 12) % 12); dirty(); }
static void gc_scale(int32_t d) { ic(&P.scale, d, 0, NSCALE - 1); }
static void gc_clk(int32_t d) { T.clk_mode = (uint8_t)clamp(T.clk_mode + d, 0, NCK - 1); P.clk_mode = T.clk_mode; dirty(); }
static void gc_clko(int32_t d) { T.clk_send = d > 0; P.clk_send = T.clk_send; dirty(); }
static void gc_et(int32_t d) { ic(&P.echo_time, d, 0, 127); project_apply(); }
static void gc_ef(int32_t d) { ic(&P.echo_fb, d, 0, 127); project_apply(); }
static void gc_slot(int32_t d) { set.slot = (uint8_t)clamp(set.slot + d, 0, NSLOTS - 1); settings_save(); }
static void gc_ble(int32_t d) { set.ble_off = d < 0; plat_ble_enable(!set.ble_off); settings_save(); }
static void ga_save(void)
{
    if (T.playing) {
        U.save_wait = 1;
        app_say("SAVES", "AT THE STOP");
    } else {
        project_save_now();
    }
}
static void ga_load(void)
{
    if (T.playing)
        transport_wait_stop();
    if (slot_load(set.slot, PROJ_MAGIC, &P, sizeof P)) {
        project_apply();
        U.dirty = 0;
        app_say("LOADED", "");
    } else {
        app_say("SLOT", "EMPTY");
    }
}
static void ga_new(void)
{
    if (T.playing)
        transport_wait_stop();
    project_new();
    project_apply();
    dirty();
    app_say("NEW", "PROJECT");
}
static const row_def_t SETUP_ROWS[] = {
    {"TEMPO", gv_tempo, gc_tempo, 0}, {"ROOT", gv_root, gc_root, 0},   {"SCALE", gv_scale, gc_scale, 0},
    {"CLOCK", gv_clk, gc_clk, 0},     {"CLOCK OUT", gv_clko, gc_clko, 0}, {"ECHO TIME", gv_et, gc_et, 0},
    {"ECHO FEEDBACK", gv_ef, gc_ef, 0}, {"SLOT", gv_slot, gc_slot, 0},  {"SAVE TO SLOT", 0, 0, ga_save},
    {"LOAD SLOT", 0, 0, ga_load},     {"NEW PROJECT", 0, 0, ga_new},    {"BLUETOOTH", gv_ble, gc_ble, 0},
};
#define NSETUP (sizeof SETUP_ROWS / sizeof SETUP_ROWS[0])

/* ---- editing ---- */
static void stage_knob(uint32_t k, int32_t d)
{
    stage_t *s = stg();
    switch (k) {
    case 0: s->deg = (int8_t)clamp(s->deg + d, -7, 21); break;
    case 1: s->pulses = (uint8_t)clamp(s->pulses + d, 1, 8); break;
    case 2: s->gate = (uint8_t)clamp(s->gate + d, 0, NGM - 1); break;
    default: s->ratchet = (uint8_t)clamp(s->ratchet + d, 1, 4); break;
    }
    dirty();
}
static void line_knob(uint32_t k, int32_t d)
{
    line_t *l = ln();
    switch (k) {
    case 0: l->dir = (uint8_t)clamp(l->dir + d, 0, NDIR - 1); break;
    case 1: l->n = (uint8_t)clamp(l->n + d, 1, NSTAGE); break;
    case 2: l->div = (uint8_t)clamp(l->div + d, 0, NPDIV - 1); break;
    default: l->acc_step = (int8_t)clamp(l->acc_step + d, -4, 4); break;
    }
    dirty();
}

static void audition(uint32_t li, uint32_t si)
{
    int32_t n = scale_deg(P.line[li].s[si].deg, P.root, P.scale, 48 + 12 * P.line[li].octave);
    if (T.playing)
        return;
    live_note(9, &P.ins[li], n, 90, 1);
    live_note(9, &P.ins[li], n, 0, 0);
}

static void rec_write(int32_t note)                     /* REC: a note into the next stage, as a degree */
{
    int32_t best = 0, bd = 999, d;
    for (d = -7; d <= 21; d++) {
        int32_t x = scale_deg(d, P.root, P.scale, 48 + 12 * ln()->octave) - note;
        if (x < 0) x = -x;
        if (x < bd) {
            bd = x;
            best = d;
        }
    }
    U.stage = U.rec_at;
    stg()->deg = (int8_t)best;
    if (stg()->gate == GM_MUTE)
        stg()->gate = GM_ONCE;
    U.rec_at = (uint8_t)((U.rec_at + 1u) % (ln()->n ? ln()->n : 1u));
    dirty();
}

static void input(void)
{
    uint32_t k;
    kit_input();
    if (DOWN(B_PLAY))
        T.want = T.playing ? 2 : 1;
    if (DOWN(B_REC)) {
        U.rec = !U.rec;
        U.rec_at = U.stage;
        app_say("REC", U.rec ? "KEYS WRITE THE STAGES" : "OFF");
    }
    if (DOWN(B_HOME)) U.page = PG_LINES;
    if (DOWN(B_EDIT)) U.page = U.page == PG_SOUND ? PG_LINES : PG_SOUND;
    if (TAPPED(B_GLO)) U.page = U.page == PG_SETUP ? PG_LINES : PG_SETUP;
    if (DOWN(B_FX)) {
        R.resync = 1;
        app_say("RE-SYNC", "");
    }
    if (DOWN(B_LFO)) {
        ln()->muted ^= 1u;
        dirty();
    }
    if (DOWN(B_ARP)) {
        uint32_t i;
        U.undo = *ln();
        U.undo_ok = 1;
        for (i = 0; i < NSTAGE; i++)
            ln()->s[i].deg = (int8_t)((int32_t)(xs32(&R.seed) % 12u) - 2);
        dirty();
        app_say("DICE", "SEQ: UNDO");
    }
    if (DOWN(B_SEQ) && U.undo_ok) {
        line_t cur = *ln();
        *ln() = U.undo;
        U.undo = cur;
        dirty();
        app_say("UNDO", "");
    }
    if (TAPPED(B_SAVE))
        ga_save();
    if (DOWN(B_OCTDN) || DOWN(B_OCTUP)) {
        ln()->octave = (int8_t)clamp(ln()->octave + (DOWN(B_OCTUP) ? 1 : -1), -2, 2);
        dirty();
    }
    if (midi_rec_note >= 0) {
        if (U.rec)
            rec_write(midi_rec_note);
        midi_rec_note = -1;
    }
    if (K.enc[EN_ALGO]) {
        U.line = (uint8_t)clamp(U.line + K.enc[EN_ALGO], 0, NLINE - 1);
        sel_line = U.line;
    }
    if (U.page == PG_SOUND) {
        kit_list_input(SOUND_ROWS, NSOUND_ROWS, &U.srow);
    } else if (U.page == PG_SETUP) {
        kit_list_input(SETUP_ROWS, NSETUP, &U.grow);
    } else {
        if (HELD(B_SEL) && (K.enc[EN_PRESET] || K.enc[EN_SELECT])) {
            kit_use(B_SEL);
            if (K.enc[EN_PRESET]) gc_scale(K.enc[EN_PRESET]);
            if (K.enc[EN_SELECT]) gc_root(K.enc[EN_SELECT]);
            app_say(ROOT_NM[P.root % 12u], SCALE_NM[P.scale % NSCALE]);
        } else {
            if (K.enc[EN_SELECT]) {
                stage_knob(0, K.enc[EN_SELECT]);
                audition(U.line, U.stage);
            }
            if (K.enc[EN_PRESET])
                stage_knob(1, K.enc[EN_PRESET]);
        }
        for (k = 0; k < 4u; k++)
            if (K.enc[EN_K1 + k]) {
                if (U.held >= 0)
                    stage_knob(k, K.enc[EN_K1 + k]);
                else
                    line_knob(k, K.enc[EN_K1 + k]);
            }
    }
    for (k = 0; k < NKEYS; k++) {
        int w = white_of(k), b = black_of(k);
        if (K.kdown >> k & 1u) {
            if (U.rec && w >= 0) {                      /* REC: the keys are a keyboard */
                int32_t n = 53 + (int32_t)k;
                rec_write(n);
                live_note(9, &P.ins[U.line], n, K.vel[k], 1);
                U.key_note[k] = (int16_t)n;
            } else if (w >= 0) {
                U.line = (uint8_t)(w / 8);
                U.stage = (uint8_t)(w % 8);
                sel_line = U.line;
                U.held = (int8_t)w;
                audition(U.line, U.stage);
            } else if (b >= 0 && b < 5) {
                stg()->gate = (uint8_t)b;
                dirty();
            } else if (b == 5) {
                stg()->slide ^= 1u;
                dirty();
            } else if (b == 6) {
                stg()->skip ^= 1u;
                dirty();
            } else if (b == 7) {
                stg()->accent ^= 1u;
                dirty();
            }
        }
        if (K.kup >> k & 1u) {
            if (U.key_note[k] >= 0) {
                live_note(9, &P.ins[U.line], U.key_note[k], 0, 0);
                U.key_note[k] = -1;
            }
            if (w >= 0 && U.held == w)
                U.held = -1;
        }
    }
    kit_cards(CARD_TEXT);
}

/* ---- the screen: each line as terraces, a stage as wide as its pulses and as high as its pitch ---- */
static void draw_line(uint32_t li, int32_t y, int32_t h)
{
    const line_t *l = &P.line[li];
    uint32_t i, total = 0, n = l->n ? l->n : 1u;
    int32_t x = 6, w = 228, lo = 99, hi = -99;
    uint16_t c = l->muted ? K_FAINT : LINE_C[li];
    char t[40];
    for (i = 0; i < NSTAGE; i++) {
        total += l->s[i].pulses;
        if (l->s[i].deg < lo) lo = l->s[i].deg;
        if (l->s[i].deg > hi) hi = l->s[i].deg;
    }
    if (hi - lo < 7)
        hi = lo + 7;
    t[0] = (char)('A' + li);
    t[1] = 0;
    cv_text(6, y, &FONT_B, t, c);
    s_cpy(t, DIR_NM[l->dir % NDIR]);
    s_cat(t, "  ");
    i_cat(t, (int32_t)n);
    s_cat(t, " ST  ");
    s_cat(t, PDIV_NM[l->div % NPDIV]);
    if (l->acc_step) {
        s_cat(t, "  ACC ");
        sgn_cat(t, l->acc_step);
        if (R.l[li].acc) {
            s_cat(t, " (");
            sgn_cat(t, R.l[li].acc);
            s_cat(t, ")");
        }
    }
    if (l->muted)
        s_cat(t, "  MUTE");
    cv_text(22, y + 2, &FONT_T, t, li == U.line ? K_FG : K_DIM);
    y += 18;
    h -= 18;
    for (i = 0; i < NSTAGE; i++) {
        const stage_t *s = &l->s[i];
        int32_t sw = (int32_t)s->pulses * w / (int32_t)total, sh = 6 + (s->deg - lo) * (h - 22) / (hi - lo);
        int active = i < n, now = T.playing && R.l[li].stage == (int8_t)i, sel = li == U.line && i == U.stage;
        uint16_t fill = !active || s->skip ? K_LINE : s->gate == GM_MUTE ? K_PANEL : c;
        int32_t top = y + h - 12 - sh;
        uint32_t p;
        cv_rect(x + 1, top, sw - 2, sh, fill);
        for (p = 1; p < s->pulses; p++)                 /* the pulses, as notches */
            cv_rect(x + (int32_t)p * sw / (int32_t)s->pulses, top, 1, 4, K_BG);
        if (now) {
            int32_t px = x + (int32_t)R.l[li].pulse * sw / (int32_t)s->pulses;
            cv_rect(x + 1, top - 3, sw - 2, 2, K_FG);
            cv_rect(px - (R.l[li].pulse ? sw / (int32_t)s->pulses : 0), y + h - 10, sw / (int32_t)s->pulses, 2, K_FG);
        }
        if (sel)
            box(x, top - 5, sw, sh + 6, K_FG);
        t[0] = GM_CH[s->gate % NGM];
        t[1] = s->ratchet > 1u ? (char)('0' + s->ratchet) : 0;
        t[2] = 0;
        if (s->slide)
            s_cat(t, "/");
        if (s->accent)
            s_cat(t, "!");
        cv_text(x + 2, y + h - 12, &FONT_T, s->skip ? "SKIP" : t, sel ? K_FG : K_DIM);
        x += sw;
    }
}

static void draw(void)
{
    static const char *const PGN[] = {"STAGES", "SOUND", "SETUP"};
    kit_header("TERRACE-8", PGN[U.page], ACCENT, T.playing, U.rec, T.bpm10);
    cv_begin(240, 200, K_BG);
    if (K.card < NB && CARD_TEXT[K.card]) {
        kit_card(K.card, CARD_TEXT[K.card], 200, ACCENT);
    } else if (U.page == PG_SOUND) {
        char t[16] = "LINE ";
        t[5] = (char)('A' + U.line);
        t[6] = 0;
        s_cat(t, " SOUND");
        cv_text(6, 0, &FONT_T, t, LINE_C[U.line]);
        kit_list_draw(SOUND_ROWS, NSOUND_ROWS, U.srow, 14, 184, ACCENT);
    } else if (U.page == PG_SETUP) {
        cv_text(6, 0, &FONT_T, "SETUP", ACCENT);
        kit_list_draw(SETUP_ROWS, NSETUP, U.grow, 14, 184, ACCENT);
    } else {
        const stage_t *s = stg();
        char t[48], n[8];
        draw_line(0, 0, 84);
        draw_line(1, 86, 84);
        s_cpy(t, "STAGE ");
        t[6] = (char)('A' + U.line);
        t[7] = (char)('1' + U.stage);
        t[8] = 0;
        s_cat(t, "  ");
        note_name(scale_deg(s->deg, P.root, P.scale, 48 + 12 * ln()->octave), n);
        s_cat(t, n);
        s_cat(t, "  x");
        i_cat(t, s->pulses);
        s_cat(t, "  ");
        s_cat(t, GM_NM[s->gate % NGM]);
        cv_rect(0, 174, 240, 1, K_LINE);
        cv_text(6, 177, &FONT_XS, t, U.held >= 0 ? K_FG : K_DIM);
        s_cpy(t, ROOT_NM[P.root % 12u]);
        s_cat(t, " ");
        s_cat(t, SCALE_NM[P.scale % NSCALE]);
        text_r(234, 179, &FONT_T, t, K_DIM);
    }
    cv_blit(0, 22);
    if (U.page != PG_LINES)
        kit_knobs("VALUE", "", "", "", ACCENT);
    else if (U.held >= 0) {
        char a[16] = "PITCH ", b[12] = "PULSES ", d[12] = "RATCHET ";
        sgn_cat(a, stg()->deg);
        i_cat(b, stg()->pulses);
        i_cat(d, stg()->ratchet);
        kit_knobs(a, b, GM_NM[stg()->gate % NGM], d, K_FG);
    } else {
        char b[12] = "STAGES ", d[12] = "ACC ";
        i_cat(b, ln()->n);
        sgn_cat(d, ln()->acc_step);
        kit_knobs(DIR_NM[ln()->dir % NDIR], b, PDIV_NM[ln()->div % NPDIV], d, LINE_C[U.line]);
    }
}

static void lights(void)
{
    uint32_t btn = 0, keys = K.keys, dkeys = 0, li, i;
    if (T.playing)
        btn |= 1u << B_PLAY | LED_PLAY_GREEN;
    if (U.rec)
        btn |= 1u << B_REC;
    if (U.dirty)
        btn |= 1u << B_SAVE;
    if (ln()->muted)
        btn |= 1u << B_LFO;
    if (U.page == PG_SOUND)
        btn |= 1u << B_EDIT;
    for (li = 0; li < NLINE; li++)                      /* white keys: stages in play dim, the playing one lit */
        for (i = 0; i < NSTAGE; i++) {
            uint32_t key = WKEY[li * 8u + i];
            if (T.playing && R.l[li].stage == (int8_t)i)
                keys |= 1u << key;
            else if (i < P.line[li].n && !P.line[li].s[i].skip)
                dkeys |= 1u << key;
        }
    if (stg()->gate < 5u)                               /* black keys: the stage's gate, slide, skip, accent */
        keys |= 1u << BKEY[stg()->gate];
    if (stg()->slide) dkeys |= 1u << BKEY[5];
    if (stg()->skip) dkeys |= 1u << BKEY[6];
    if (stg()->accent) dkeys |= 1u << BKEY[7];
    plat_lights(btn, keys, 0, dkeys, 1);
}
