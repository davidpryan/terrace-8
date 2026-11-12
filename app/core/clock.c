/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Ryan */
/* core/clock.c: the transport, run inside the audio render so a step lands on its exact sample.
 * 96 ticks a quarter note. Each block is cut at the samples where ticks fall: the voices are rendered up to the tick,
 * then the app's seq_tick() runs (and may start voices), then the rest. Notes go to the synth and to MIDI (USB and
 * Bluetooth) together, and their offs wait in a queue by sample time.
 * Clock: INTERNAL (and send MIDI clock, start, stop if clk_send) or FOLLOW (a MIDI clock coming in: each F8 is four
 * ticks, spread over the time the last one took).
 * The app provides: seq_tick(), seq_started(), seq_stopped(), seq_midi_note(note, vel, chan) (MIDI in, render). */

#define PPQN 96u
enum { CK_INTERNAL, CK_FOLLOW, NCK };
static const char *const CK_NM[NCK] = {"INTERNAL", "FOLLOW"};

static void seq_tick(void);
static void seq_started(void);
static void seq_stopped(void);
static void seq_midi_note(int32_t note, uint32_t vel, uint32_t chan);

static struct {
    volatile uint8_t playing;
    volatile uint8_t want;                              /* the main loop asks: 1 start, 2 stop, 3 continue */
    uint8_t clk_mode, clk_send;
    uint16_t bpm10;                                     /* 30.0 .. 300.0 */
    volatile uint32_t tick;                             /* ticks since PLAY */
    volatile uint32_t now;                              /* samples since boot */
    float acc;                                          /* samples until the next tick */
    /* following */
    uint32_t f8_last, f8_gap;                           /* samples: the last F8, and the smoothed gap */
    uint8_t owed;                                       /* ticks of the last F8 still to come */
    float owed_acc;
    uint32_t f8_seen;
} T = {0, 0, CK_INTERNAL, 1, 1200, 0, 0, 0.0f, 0, 0, 0, 0.0f, 0};

#define MIDI_TO (TO_USB | TO_BLE)
static uint32_t midi_sent;
static void midi_out(uint32_t st, uint32_t d1, uint32_t d2)
{
    plat_midi_send((st >> 4) | st << 8 | (d1 & 0x7Fu) << 16 | (d2 & 0x7Fu) << 24, MIDI_TO);
    midi_sent++;
}
static void midi_rt(uint32_t b) { plat_midi_send(0x0Fu | b << 8, MIDI_TO); }

/* ---- notes and their offs ---- */
typedef struct {
    uint32_t due;                                       /* sample time */
    uint8_t owner, chan;                                /* chan 1..16, 0 none */
    int8_t note;
    uint8_t used;
} off_t_;
#define NOFF 96u
static off_t_ OFFQ[NOFF];

static void off_now(off_t_ *o)
{
    voice_off(o->owner, o->note);
    if (o->chan)
        midi_out(0x80u | (o->chan - 1u), (uint32_t)o->note, 0);
    o->used = 0;
}

static void offs_due(void)
{
    uint32_t i;
    for (i = 0; i < NOFF; i++)
        if (OFFQ[i].used && (int32_t)(T.now - OFFQ[i].due) >= 0)
            off_now(&OFFQ[i]);
}

static void offs_all(void)                              /* every note owed its off: now */
{
    uint32_t i;
    for (i = 0; i < NOFF; i++)
        if (OFFQ[i].used)
            off_now(&OFFQ[i]);
}

static void offs_owner(uint32_t owner)                  /* a mono part's last note ends as the next starts */
{
    uint32_t i;
    for (i = 0; i < NOFF; i++)
        if (OFFQ[i].used && OFFQ[i].owner == owner)
            off_now(&OFFQ[i]);
}
static void offs_owner_legato(uint32_t owner)           /* the same, but the voice keeps its gate: it glides */
{
    uint32_t i;
    for (i = 0; i < NOFF; i++)
        if (OFFQ[i].used && OFFQ[i].owner == owner) {
            if (OFFQ[i].chan)
                midi_out(0x80u | (OFFQ[i].chan - 1u), (uint32_t)OFFQ[i].note, 0);
            OFFQ[i].used = 0;
        }
}

static float samples_per_tick(void) { return SR * 60.0f / ((float)T.bpm10 * 0.1f * (float)PPQN); }
static uint32_t ticks_to_samples(uint32_t t) { return (uint32_t)((float)t * samples_per_tick()); }

/* a note: to the synth and MIDI; it ends gate samples later (0: it is held until note_end) */
static uint8_t heard[16];                                /* parts that sounded lately (the screen's meters) */
static void play_note(uint32_t owner, const patch_t *p, int32_t note, uint32_t vel, uint32_t gate, int mono)
{
    uint32_t i;
    if (note < 0 || note > 127)
        return;
    if (vel > 127u)
        vel = 127u;
    if (mono && p->glide && p->type == PT_SYNTH)
        offs_owner_legato(owner);
    else if (mono)
        offs_owner(owner);
    voice_on(owner, p, note, vel, mono);
    if (p->chan)
        midi_out(0x90u | (p->chan - 1u), (uint32_t)(note + (p->type == PT_DRUM ? 0 : 12 * p->octave)), vel);
    heard[owner & 15u] = 8;
    if (!gate)
        return;
    note += p->type == PT_DRUM ? 0 : 12 * p->octave;    /* the off as MIDI heard the on */
    for (i = 0; i < NOFF && OFFQ[i].used; i++)
        ;
    if (i == NOFF) {                                    /* the queue is full: the first owed off goes now */
        off_now(&OFFQ[0]);
        i = 0;
    }
    OFFQ[i].used = 1;
    OFFQ[i].due = T.now + gate;
    OFFQ[i].owner = (uint8_t)owner;
    OFFQ[i].chan = p->chan;
    OFFQ[i].note = (int8_t)note;
}

static void note_end(uint32_t owner, const patch_t *p, int32_t note)   /* a held note (gate 0) let go */
{
    voice_off(owner, note);
    if (p->chan)
        midi_out(0x80u | (p->chan - 1u), (uint32_t)(note + (p->type == PT_DRUM ? 0 : 12 * p->octave)), 0);
}

/* ---- the main loop's notes (keys played live), handed to the render ---- */
typedef struct {
    uint8_t owner, on, vel;
    int8_t note;
    const patch_t *p;
} live_t;
#define NLIVE 32u
static live_t LIVEQ[NLIVE];
static volatile uint32_t live_w, live_r;
static void live_note(uint32_t owner, const patch_t *p, int32_t note, uint32_t vel, int on)
{
    live_t *l;
    if (live_w - live_r >= NLIVE)
        return;
    l = &LIVEQ[live_w % NLIVE];
    l->owner = (uint8_t)owner;
    l->on = (uint8_t)on;
    l->vel = (uint8_t)vel;
    l->note = (int8_t)note;
    l->p = p;
    live_w++;
}
#ifdef APP_LIVE_HOOK
static void live_hook(const live_t *l);                 /* the app sees each live note too (SPOOL-4 records them) */
#endif
static void live_run(void)
{
    while (live_r != live_w) {
        live_t *l = &LIVEQ[live_r % NLIVE];
        if (l->on)
            play_note(l->owner, l->p, l->note, l->vel, 0, 0);
        else
            note_end(l->owner, l->p, l->note);
#ifdef APP_LIVE_HOOK
        live_hook(l);
#endif
        live_r++;
    }
}

/* ---- transport ---- */
static void transport_start(void)
{
    T.tick = 0;
    T.acc = 0.0f;
    T.playing = 1;
    seq_started();
    if (T.clk_send && T.clk_mode == CK_INTERNAL)
        midi_rt(0xFAu);
}
static void transport_stop(void)
{
    T.playing = 0;
    T.owed = 0;
    offs_all();
    seq_stopped();
    if (T.clk_send && T.clk_mode == CK_INTERNAL)
        midi_rt(0xFCu);
}

static void do_tick(void)
{
    if (T.clk_send && T.clk_mode == CK_INTERNAL && T.tick % 4u == 0u)
        midi_rt(0xF8u);
    seq_tick();
    T.tick++;
}

static void midi_in_run(void)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24 & 0x7Fu;
        if (st == 0xF8u) {
            uint32_t gap = T.now - T.f8_last;
            T.f8_last = T.now;
            T.f8_seen++;
            if (gap > 40u && gap < 20000u)
                T.f8_gap = T.f8_gap ? (T.f8_gap * 3u + gap) / 4u : gap;
            if (T.clk_mode == CK_FOLLOW && T.playing) {
                while (T.owed) {                       /* the last pulse's ticks not yet played: now */
                    T.owed--;
                    do_tick();
                }
                do_tick();
                T.owed = 3;
                T.owed_acc = 0.0f;
                if (T.f8_gap)                          /* and the tempo shown */
                    T.bpm10 = (uint16_t)clamp((int32_t)(SR * 60.0f * 10.0f / ((float)T.f8_gap * 24.0f) + 0.5f), 300, 3000);
            }
        } else if (st == 0xFAu || st == 0xFBu) {
            if (T.clk_mode == CK_FOLLOW) {
                if (st == 0xFAu || !T.playing)
                    transport_start();
            }
        } else if (st == 0xFCu) {
            if (T.clk_mode == CK_FOLLOW && T.playing)
                transport_stop();
        } else if ((st & 0xF0u) == 0x90u && d2) {
            seq_midi_note((int32_t)d1, d2, st & 15u);
        } else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) {
            seq_midi_note((int32_t)d1, 0, st & 15u);
        }
    }
}

/* the render: the transport and the voices, n frames into lr */
static void clock_render(int32_t *lr, uint32_t n, float gain)
{
    uint32_t done = 0, i;
    for (i = 0; i < 2u * n; i++)
        lr[i] = 0;
    if (T.want) {
        uint32_t w = T.want;
        T.want = 0;
        if (w == 1u && T.clk_mode == CK_INTERNAL)
            transport_start();
        else if (w == 2u && T.playing)
            transport_stop();
    }
    midi_in_run();
    live_run();
    while (done < n) {
        uint32_t run = n - done;
        int tick = 0;
        if (T.playing) {
            if (T.clk_mode == CK_INTERNAL) {
                if (T.acc < 1.0f) {
                    tick = 1;
                    run = 0;
                } else if ((float)run > T.acc) {
                    run = (uint32_t)T.acc;
                }
            } else if (T.owed && T.f8_gap) {
                float step = (float)T.f8_gap / 4.0f;
                if (T.owed_acc + (float)run >= step) {
                    run = (uint32_t)(step - T.owed_acc);
                    if (run == 0u) {
                        tick = 2;
                    }
                }
            }
        }
        if (tick == 1) {
            do_tick();
            T.acc += samples_per_tick();
            offs_due();
            continue;
        }
        if (tick == 2) {
            T.owed--;
            T.owed_acc = 0.0f;
            do_tick();
            continue;
        }
        if (run) {
            synth_render(lr, done, run, gain);
            done += run;
            T.now += run;
            if (T.playing && T.clk_mode == CK_INTERNAL)
                T.acc -= (float)run;
            if (T.owed)
                T.owed_acc += (float)run;
            offs_due();
        }
    }
}
