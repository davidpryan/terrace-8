/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Ryan */
/* TERRACE-8: time is part of the step. Two lines of eight STAGES, after the Intellijel Metropolis and the Moog 960:
 * each stage has a pitch (a scale degree), a number of PULSES it lasts (1..8), a GATE mode (silent, once, every
 * pulse, held through, every other pulse), RATCHETS on each pulse it plays, a SLIDE into it and a SKIP. Eight stages
 * can make a phrase of 64 pulses, and the two lines (of different lengths) cross against each other.
 * A line runs forward, backward, back and forth, at random or by a random walk; its ACCUMULATOR moves the whole line
 * up (or down) by some degrees each time round, back to the start past its limit. */

#define NLINE 2u
#define NSTAGE 8u
enum { GM_MUTE, GM_ONCE, GM_EVERY, GM_HOLD, GM_ALT, NGM };
static const char *const GM_NM[NGM] = {"SILENT", "ONCE", "EVERY", "HOLD", "EVERY OTHER"};
static const char GM_CH[NGM] = {'-', '1', 'E', 'H', 'A'};
enum { DIR_FWD, DIR_REV, DIR_PING, DIR_RAND, DIR_WALK, NDIR };
static const char *const DIR_NM[NDIR] = {"FORWARD", "BACKWARD", "PING-PONG", "RANDOM", "RANDOM WALK"};
static const uint16_t PDIV_T[] = {12, 16, 24, 32, 48, 96};
static const char *const PDIV_NM[] = {"1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4"};
#define NPDIV 6u

typedef struct {
    int8_t deg;                                         /* scale degree, -7..21 */
    uint8_t pulses;                                     /* 1..8 */
    uint8_t gate;                                       /* GM_* */
    uint8_t ratchet;                                    /* 1..4 hits a pulse */
    uint8_t slide, skip, accent, rsv;
} stage_t;

typedef struct {
    stage_t s[NSTAGE];
    uint8_t n;                                          /* stages in play, 1..8 */
    uint8_t dir, div, gate_len;                         /* gate_len: % of a pulse for ONCE / EVERY */
    int8_t octave, acc_step;                            /* accumulator: degrees added each time round */
    uint8_t acc_limit, muted;
} line_t;

#define PROJ_MAGIC 0x38524554u                          /* "TER8" */
#define PROJ_VER 1u
typedef struct {
    uint16_t bpm10;
    uint8_t root, scale, clk_mode, clk_send, echo_time, echo_fb;
    line_t line[NLINE];
    patch_t ins[NLINE];
    uint8_t rsv[24];
} proj_t;
static proj_t P;

static struct {
    struct {
        int8_t stage;                                   /* the stage playing (-1 before the first) */
        int8_t dir;                                     /* ping-pong's way */
        uint8_t pulse;                                  /* pulses into the stage */
        uint32_t t;                                     /* ticks into the pulse */
        int8_t acc;                                     /* the accumulator now */
        uint8_t rat_left, rat_gap, rat_t;
        int16_t note;                                   /* sounding */
        uint32_t passes;
    } l[NLINE];
    uint32_t seed;
    volatile uint8_t resync;
} R;

static int32_t stage_note(uint32_t li, uint32_t si)
{
    const line_t *ln = &P.line[li];
    return scale_deg(ln->s[si].deg + R.l[li].acc, P.root, P.scale, 48 + 12 * ln->octave);
}

/* the next stage by the line's direction (skipped stages passed over; all skipped: stay) */
static void next_stage(uint32_t li)
{
    line_t *ln = &P.line[li];
    uint32_t n = ln->n ? ln->n : 1u, tries;
    int32_t s = R.l[li].stage;
    for (tries = 0; tries < n * 2u; tries++) {
        int wrapped = 0;
        switch (ln->dir) {
        case DIR_REV:
            s = s <= 0 ? (int32_t)n - 1 : s - 1;
            wrapped = s == (int32_t)n - 1;
            break;
        case DIR_PING:
            if (R.l[li].dir == 0)
                R.l[li].dir = 1;
            s += R.l[li].dir;
            if (s >= (int32_t)n || s < 0) {
                R.l[li].dir = (int8_t)-R.l[li].dir;
                s += 2 * R.l[li].dir;
                s = clamp(s, 0, (int32_t)n - 1);
                wrapped = s <= 1;
            }
            break;
        case DIR_RAND:
            s = (int32_t)(xs32(&R.seed) % n);
            wrapped = s == 0;
            break;
        case DIR_WALK:
            s += (xs32(&R.seed) & 1u) ? 1 : -1;
            if (s < 0) s = 1 < (int32_t)n ? 1 : 0;
            if (s >= (int32_t)n) s = (int32_t)n - 2 < 0 ? 0 : (int32_t)n - 2;
            wrapped = s == 0;
            break;
        default:
            s = s + 1 >= (int32_t)n ? 0 : s + 1;
            wrapped = s == 0;
            break;
        }
        if (wrapped && R.l[li].stage >= 0) {           /* round again: the accumulator */
            R.l[li].passes++;
            if (ln->acc_step) {
                int32_t a = R.l[li].acc + ln->acc_step;
                if (a > (int32_t)ln->acc_limit || a < -(int32_t)ln->acc_limit)
                    a = 0;
                R.l[li].acc = (int8_t)a;
            }
        }
        if (!ln->s[s].skip)
            break;
    }
    R.l[li].stage = (int8_t)s;
    R.l[li].pulse = 0;
}

static void line_hit(uint32_t li, uint32_t vel, uint32_t len_ticks)
{
    const line_t *ln = &P.line[li];
    const stage_t *st = &ln->s[R.l[li].stage];
    patch_t pt;
    int32_t n;
    if (ln->muted)
        return;
    n = stage_note(li, (uint32_t)R.l[li].stage);
    pt = P.ins[li];
    pt.glide = st->slide ? (uint8_t)(pt.glide ? pt.glide : 40u) : 0u;
    /* the patch copy lives only as long as the call: play_note reads it at once (the MIDI channel and the voice) */
    play_note(li, &pt, n, vel, ticks_to_samples(len_ticks), 1);
    R.l[li].note = (int16_t)n;
}

static void pulse(uint32_t li)                          /* a pulse of the line: the stage decides what sounds */
{
    line_t *ln = &P.line[li];
    const stage_t *st;
    uint32_t pt = PDIV_T[ln->div % NPDIV], len, vel, play = 0;
    if (R.l[li].stage < 0 || R.l[li].pulse >= P.line[li].s[R.l[li].stage].pulses)
        next_stage(li);
    st = &ln->s[R.l[li].stage];
    vel = st->accent ? 122u : 90u;
    switch (st->gate) {
    case GM_ONCE: play = R.l[li].pulse == 0u; break;
    case GM_EVERY: play = 1; break;
    case GM_HOLD: play = R.l[li].pulse == 0u; break;
    case GM_ALT: play = (R.l[li].pulse & 1u) == 0u; break;
    default: play = 0; break;
    }
    if (play) {
        uint32_t r = st->ratchet ? st->ratchet : 1u;
        len = st->gate == GM_HOLD ? pt * st->pulses - 2u : pt * ln->gate_len / 100u + 1u;
        if (r > 1u)
            len = pt / r * 2u / 3u + 1u;
        line_hit(li, vel, len);
        R.l[li].rat_left = (uint8_t)(r - 1u);
        R.l[li].rat_gap = (uint8_t)(pt / r);
        R.l[li].rat_t = 0;
    }
    R.l[li].pulse++;
}

static void seq_tick(void)
{
    uint32_t li;
    if (R.resync) {
        R.resync = 0;
        for (li = 0; li < NLINE; li++) {
            R.l[li].stage = -1;
            R.l[li].t = 0;
            R.l[li].acc = 0;
        }
    }
    for (li = 0; li < NLINE; li++) {
        uint32_t pt = PDIV_T[P.line[li].div % NPDIV];
        if (R.l[li].t == 0u)
            pulse(li);
        else if (R.l[li].rat_left && ++R.l[li].rat_t >= R.l[li].rat_gap) {
            const stage_t *st = &P.line[li].s[R.l[li].stage];
            R.l[li].rat_t = 0;
            R.l[li].rat_left--;
            line_hit(li, st->accent ? 100u : 70u, R.l[li].rat_gap * 2u / 3u + 1u);
        }
        if (++R.l[li].t >= pt)
            R.l[li].t = 0;
    }
}

static void seq_started(void)
{
    uint32_t li;
    for (li = 0; li < NLINE; li++) {
        R.l[li].stage = -1;
        R.l[li].pulse = 0;
        R.l[li].t = 0;
        R.l[li].acc = 0;
        R.l[li].dir = 1;
        R.l[li].rat_left = 0;
        R.l[li].note = -1;
        R.l[li].passes = 0;
    }
}
static void seq_stopped(void)
{
    uint32_t li;
    for (li = 0; li < NLINE; li++)
        R.l[li].note = -1;
}

/* MIDI in: plays line A's sound, snapped to the key (or, in REC, writes the stage: ui.c reads midi_rec_note) */
static volatile int16_t midi_rec_note = -1;
static volatile uint8_t sel_line;
static void seq_midi_note(int32_t note, uint32_t vel, uint32_t chan)
{
    (void)chan;
    if (vel) {
        play_note(9, &P.ins[sel_line % NLINE], note, vel, 0, 0);
        midi_rec_note = (int16_t)note;
    } else {
        note_end(9, &P.ins[sel_line % NLINE], note);
    }
}

static void line_default(line_t *ln, const int8_t *deg, const uint8_t *pul, const uint8_t *gate)
{
    uint32_t i;
    ln->n = NSTAGE;
    ln->dir = DIR_FWD;
    ln->div = 2;
    ln->gate_len = 60;
    ln->octave = 0;
    ln->acc_step = 0;
    ln->acc_limit = 7;
    ln->muted = 0;
    for (i = 0; i < NSTAGE; i++) {
        ln->s[i].deg = deg[i];
        ln->s[i].pulses = pul[i];
        ln->s[i].gate = gate[i];
        ln->s[i].ratchet = 1;
        ln->s[i].slide = 0;
        ln->s[i].skip = 0;
        ln->s[i].accent = i == 0u;
    }
}

static void project_new(void)
{
    static const int8_t DA[8] = {0, 2, 4, 7, 6, 4, 3, 1}, DB[8] = {-7, -7, -3, -4, -7, -5, -3, -2};
    static const uint8_t PA[8] = {1, 1, 2, 1, 1, 3, 1, 2}, PB[8] = {2, 1, 1, 4, 2, 1, 1, 2};
    static const uint8_t GA[8] = {GM_ONCE, GM_ONCE, GM_EVERY, GM_ONCE, GM_ONCE, GM_HOLD, GM_ONCE, GM_ALT};
    static const uint8_t GB[8] = {GM_HOLD, GM_ONCE, GM_ONCE, GM_EVERY, GM_HOLD, GM_ONCE, GM_MUTE, GM_ONCE};
    uint32_t i;
    uint8_t *b = (uint8_t *)&P;
    for (i = 0; i < sizeof P; i++)
        b[i] = 0;
    P.bpm10 = 1240;
    P.root = 2;                                         /* D dorian */
    P.scale = SC_DORIAN;
    P.clk_mode = CK_INTERNAL;
    P.clk_send = 1;
    P.echo_time = 70;
    P.echo_fb = 60;
    line_default(&P.line[0], DA, PA, GA);
    line_default(&P.line[1], DB, PB, GB);
    P.line[0].s[2].ratchet = 2;
    P.line[0].s[4].slide = 1;
    P.line[0].acc_step = 2;
    P.line[0].acc_limit = 4;
    P.line[1].n = 7;
    P.line[1].s[3].ratchet = 1;
    P.line[1].s[5].slide = 1;
    patch_default(&P.ins[0], PT_SYNTH, 1);
    patch_default(&P.ins[1], PT_SYNTH, 2);
    P.ins[0].wave = WV_SQR;
    P.ins[0].cutoff = 62;
    P.ins[0].reso = 60;
    P.ins[0].decay = 45;
    P.ins[0].sustain = 40;
    P.ins[0].echo = 55;
    P.ins[0].glide = 30;
    P.ins[1].octave = -1;
    P.ins[1].cutoff = 38;
    P.ins[1].reso = 95;
    P.ins[1].envmod = 85;
    P.ins[1].decay = 42;
    P.ins[1].sustain = 30;
    P.ins[1].glide = 35;
}

static void project_apply(void)
{
    T.bpm10 = P.bpm10;
    T.clk_mode = P.clk_mode;
    T.clk_send = P.clk_send;
    echo_time = 0.05f + (float)P.echo_time * (0.32f / 127.0f);
    echo_fb = (float)P.echo_fb * (0.85f / 127.0f);
}
