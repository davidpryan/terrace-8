/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Ryan */
/* core/synth.c: the sound the sequencers play when nothing else is plugged in. Eight voices, each a pair of
 * band-limited oscillators (saw, square, triangle, sine, noise) into a resonant state-variable low-pass with an ADSR
 * on both, or one of eleven synthesised drums; a shared echo. Everything runs in the audio render (core/clock.c
 * calls voice_on at the sample a tick falls on), so the timing is sample-exact.
 * A patch is 16 bytes and every value is 0..127 unless said, so the apps can show and save them as they are. */

#define SR 44100.0f
#define NVOICE 8u

enum { PT_SYNTH, PT_DRUM, PT_MIDI, NPT_TYPE };          /* PT_MIDI: silent here, MIDI out only */
enum { WV_SAW, WV_SQR, WV_TRI, WV_SINE, WV_NOISE, WV_SUPER, NWAVE };
static const char *const PT_TYPE_NM[NPT_TYPE] = {"SYNTH", "DRUM", "MIDI"};
static const char *const WAVE_NM[NWAVE] = {"SAW", "SQUARE", "TRI", "SINE", "NOISE", "SUPER"};

typedef struct {
    uint8_t type, wave;
    uint8_t cutoff, reso, envmod;
    uint8_t attack, decay, sustain, release;
    uint8_t level, detune, echo;
    uint8_t chan;                                       /* MIDI out channel 1..16, 0 = none */
    uint8_t glide;                                      /* portamento time (mono parts) */
    int8_t octave;                                      /* -3..+3 */
    uint8_t cc;                                         /* the CC a part's lane sends (0 = none) */
} patch_t;

enum { DR_KICK, DR_RIM, DR_SNARE, DR_CLAP, DR_TOM, DR_CHAT, DR_OHAT, DR_COWB, DR_CYMB, DR_SHAKE, DR_ZAP, NDRUM };
static const char *const DRUM_NM[NDRUM] = {"KICK", "RIM", "SNARE", "CLAP", "TOM", "C.HAT", "O.HAT", "COWBELL",
                                           "CYMBAL", "SHAKER", "ZAP"};
/* a drum patch plays one sound per note, as General MIDI drums lay them out from C (36 = kick, 38 snare, 42 hat) */
static const uint8_t DRUM_OF_PC[12] = {DR_KICK, DR_RIM, DR_SNARE, DR_CLAP, DR_SNARE, DR_TOM,
                                       DR_CHAT, DR_TOM, DR_SHAKE, DR_TOM, DR_OHAT, DR_CYMB};
static uint32_t drum_of(int32_t note)
{
    if (note >= 56 && note <= 57)
        return note == 56 ? DR_COWB : DR_ZAP;           /* GM 56 cowbell; 57 (crash) is the zap */
    return DRUM_OF_PC[(uint32_t)note % 12u];
}

typedef struct {
    uint8_t on;                                         /* 1 sounding (gate or release) */
    uint8_t gate;
    uint8_t stage;                                      /* 0 attack 1 decay 2 sustain 3 release */
    uint8_t drum;                                       /* PT_DRUM: the sound; 0xFF a synth voice */
    uint8_t owner;                                      /* the part that played it (mono parts reuse theirs) */
    int8_t note;
    uint16_t age;
    float env, a_inc, d_coef, s_lvl, r_coef;
    float ph1, ph2, inc1, inc2, inc_target, glide_k;
    float lp, bp, cut, res, envmod;
    float vel, lvl, echo;
    uint8_t wave;
    uint32_t t, noise;                                  /* drums: samples since the hit; the noise's state */
    float dpitch, ddec, dtone, dph2;
    float pm;                                           /* the hit's own pitch factor */
} voice_t;

static voice_t V[NVOICE];
static float screw_mul = 1.0f;                          /* every pitch times this (GLOAM's SCREW; 1 elsewhere) */
static float hit_pitch = 1.0f;                          /* the next voice_on's own pitch factor (a drum roll's fall) */
static uint16_t v_age;
static float echo_time = 0.375f, echo_fb = 0.45f;       /* seconds, feedback (the apps set them) */
#define ECHO_N 16384u                                   /* 0.37 s at 44.1 kHz */
static int16_t echo_buf[ECHO_N] PLAT_POOL;
static uint32_t echo_w;
static float echo_lp;

static float tcurve(uint32_t v)                         /* 0..127 -> 1 ms .. 4 s, exponential */
{
    return 0.001f * fm_exp2f((float)v * (11.97f / 127.0f));
}
static float coef(float sec) { return fm_expf(-1.0f / (sec * SR)); }
static float note_inc(float note) { return 440.0f * fm_exp2f((note - 69.0f) * (1.0f / 12.0f)) / SR; }

static voice_t *voice_find(uint32_t owner, int mono)
{
    uint32_t i, best = 0, best_age = 0;
    if (mono)
        for (i = 0; i < NVOICE; i++)
            if (V[i].on && V[i].owner == owner && V[i].drum == 0xFFu)
                return &V[i];
    for (i = 0; i < NVOICE; i++)
        if (!V[i].on)
            return &V[i];
    for (i = 0; i < NVOICE; i++) {                      /* all busy: the oldest released one, else the oldest */
        uint32_t a = (uint16_t)(v_age - V[i].age) + (V[i].gate ? 0u : 65536u);
        if (a > best_age) {
            best_age = a;
            best = i;
        }
    }
    return &V[best];
}

/* a note on: owner is the part (track) that plays it; mono parts glide and retrigger in one voice */
static void voice_on(uint32_t owner, const patch_t *p, int32_t note, uint32_t vel, int mono)
{
    voice_t *v;
    float lvl;
    if (p->type == PT_MIDI || vel == 0u)
        return;
    lvl = (float)p->level * (1.0f / 127.0f);
    lvl *= lvl;
    if (p->type == PT_DRUM) {
        uint32_t d = drum_of(note);
        v = voice_find(owner, 0);
        v->drum = (uint8_t)d;
        v->t = 0;
        v->noise = 74565u + (uint32_t)note * 7919u;
        v->ph1 = v->ph2 = 0.0f;
        v->ddec = 0.25f + (float)p->decay * (1.0f / 64.0f);     /* x0.25 .. x2.2 */
        v->dtone = (float)p->cutoff * (1.0f / 127.0f);
        v->dpitch = d == DR_TOM ? 60.0f + (float)(note - 36) * 6.0f : 0.0f;
        v->lp = v->bp = 0.0f;
        v->gate = 0;
        v->stage = 3;
        v->env = 1.0f;
    } else {
        int retrig;
        v = voice_find(owner, mono);
        retrig = !(mono && v->on && v->gate && p->glide);
        v->drum = 0xFFu;
        v->wave = p->wave;
        note += 12 * p->octave;
        v->inc_target = note_inc((float)note);
        if (retrig || !v->on) {
            v->inc1 = v->inc_target;
            if (!v->on) {
                v->ph1 = 0.0f;
                v->ph2 = 0.37f;
                v->lp = v->bp = 0.0f;
                v->env = 0.0f;
            }
            v->stage = 0;
        }
        v->glide_k = p->glide ? 1.0f - coef(tcurve(p->glide) * 0.25f) : 1.0f;
        v->a_inc = 1.0f / (tcurve(p->attack) * SR);
        v->d_coef = coef(tcurve(p->decay) * 0.5f);
        v->s_lvl = (float)p->sustain * (1.0f / 127.0f);
        v->r_coef = coef(tcurve(p->release) * 0.5f);
        v->cut = (float)p->cutoff * (1.0f / 127.0f);
        v->res = (float)p->reso * (1.0f / 140.0f);
        v->envmod = (float)p->envmod * (1.0f / 127.0f);
        v->inc2 = 1.0f + (float)p->detune * (0.03f / 127.0f);
        v->noise = 0x2545F491u ^ (uint32_t)note;
        v->gate = 1;
    }
    v->pm = hit_pitch;
    hit_pitch = 1.0f;
    v->owner = (uint8_t)owner;
    v->note = (int8_t)note;
    v->vel = 0.25f + 0.75f * (float)vel * (1.0f / 127.0f);
    v->lvl = lvl;
    v->echo = (float)p->echo * (1.0f / 127.0f);
    v->age = ++v_age;
    v->on = 1;
}

static void voice_off(uint32_t owner, int32_t note)     /* note -1: every note of the owner */
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (V[i].on && V[i].gate && V[i].owner == owner && V[i].drum == 0xFFu) {
            if (note >= 0) {                            /* the note as played (the patch's octave was added) */
                int32_t d = V[i].note - note;
                if (d % 12 != 0 || d < -36 || d > 36)
                    continue;
            }
            V[i].gate = 0;
            V[i].stage = 3;
        }
}

static void voices_silence(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        V[i].on = V[i].gate = 0;
}

static inline float nz(uint32_t *s)                     /* white noise, -1..1 */
{
    *s = *s * 1664525u + 1013904223u;
    return (float)(int32_t)*s * (1.0f / 2147483648.0f);
}
static inline float blep(float t, float dt)             /* polyBLEP: the step's correction near a jump */
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static float osc(voice_t *v, float ph, float inc)
{
    float s;
    switch (v->wave) {
    case WV_SQR:
        s = (ph < 0.5f ? 1.0f : -1.0f) + blep(ph, inc);
        ph += 0.5f;
        if (ph >= 1.0f)
            ph -= 1.0f;
        return s - blep(ph, inc);
    case WV_TRI: return 4.0f * fm_fabsf(ph - 0.5f) - 1.0f;
    case WV_SINE: return fm_sin_pi(ph * 2.0f - 1.0f) * -1.0f;
    case WV_NOISE: return nz(&v->noise);
    default: return 2.0f * ph - 1.0f - blep(ph, inc);
    }
}

static float drum_sample(voice_t *v)
{
    float t = (float)v->t * (1.0f / SR), s = 0.0f, e, f, n;
    float dk = v->ddec, pm = v->pm * screw_mul;
    switch (v->drum) {
    case DR_KICK:
        e = fm_expf(-t * 7.0f / dk);
        f = 45.0f + 160.0f * fm_expf(-t * 38.0f) * (0.5f + v->dtone);
        v->ph1 += f * pm / SR;
        if (v->ph1 >= 1.0f)
            v->ph1 -= 1.0f;
        s = fm_sin_pi(v->ph1 * 2.0f - 1.0f) * e * 1.3f;
        s = fm_tanhf(s * (1.0f + v->dtone * 2.0f));
        if (t > 1.6f * dk)
            v->on = 0;
        break;
    case DR_TOM:
        e = fm_expf(-t * 9.0f / dk);
        f = v->dpitch * (1.0f + 0.6f * fm_expf(-t * 30.0f));
        v->ph1 += f * pm / SR;
        if (v->ph1 >= 1.0f)
            v->ph1 -= 1.0f;
        s = fm_sin_pi(v->ph1 * 2.0f - 1.0f) * e;
        if (t > 1.0f * dk)
            v->on = 0;
        break;
    case DR_SNARE:
    case DR_RIM:
        n = nz(&v->noise);
        v->bp += 0.35f * (n - v->bp);                   /* a gentle low-pass on the rattle */
        f = v->drum == DR_RIM ? 820.0f : 185.0f + 60.0f * v->dtone;
        v->ph1 += f * pm / SR;
        if (v->ph1 >= 1.0f)
            v->ph1 -= 1.0f;
        if (v->drum == DR_RIM) {
            e = fm_expf(-t * 90.0f / dk);
            s = (fm_sin_pi(v->ph1 * 2.0f - 1.0f) * 0.8f + n * 0.3f) * e;
            if (t > 0.12f * dk)
                v->on = 0;
        } else {
            s = fm_sin_pi(v->ph1 * 2.0f - 1.0f) * fm_expf(-t * 30.0f) * 0.6f +
                (n - v->bp * (1.0f - v->dtone)) * fm_expf(-t * 16.0f / dk) * 0.7f;
            if (t > 0.5f * dk)
                v->on = 0;
        }
        break;
    case DR_CLAP: {
        float pulse = t < 0.03f ? fm_expf(-((t * 1000.0f) - fm_floorf(t * 100.0f) * 10.0f) * 0.5f) : 1.0f;
        n = nz(&v->noise);
        v->lp += 0.5f * (n - v->lp);
        v->bp += 0.12f * (v->lp - v->bp);
        s = (v->lp - v->bp) * pulse * fm_expf(-(t < 0.03f ? 0.0f : t - 0.03f) * 18.0f / dk) * 1.6f;
        if (t > 0.45f * dk)
            v->on = 0;
        break;
    }
    case DR_CHAT:
    case DR_OHAT:
    case DR_SHAKE:
    case DR_CYMB: {
        float dec = v->drum == DR_CHAT ? 60.0f : v->drum == DR_SHAKE ? 30.0f : v->drum == DR_OHAT ? 7.0f : 3.0f;
        n = nz(&v->noise);
        if (v->drum == DR_CYMB) {                       /* square partials for the metal */
            v->ph1 += 540.0f * pm / SR;
            v->ph2 += 800.0f * pm / SR;
            if (v->ph1 >= 1.0f)
                v->ph1 -= 1.0f;
            if (v->ph2 >= 1.0f)
                v->ph2 -= 1.0f;
            n = n * 0.6f + ((v->ph1 < 0.5f) ^ (v->ph2 < 0.5f) ? 0.4f : -0.4f);
        }
        v->lp += (0.55f + 0.4f * v->dtone) * (n - v->lp);
        s = (n - v->lp) * fm_expf(-t * dec / dk) * (v->drum == DR_SHAKE ? 0.7f : 1.0f);
        if (t > 5.0f / dec * dk)
            v->on = 0;
        break;
    }
    case DR_COWB:
        v->ph1 += 560.0f * pm / SR;
        v->ph2 += 845.0f * pm / SR;
        if (v->ph1 >= 1.0f)
            v->ph1 -= 1.0f;
        if (v->ph2 >= 1.0f)
            v->ph2 -= 1.0f;
        s = ((v->ph1 < 0.5f ? 0.5f : -0.5f) + (v->ph2 < 0.5f ? 0.5f : -0.5f));
        v->lp += 0.3f * (s - v->lp);
        s = v->lp * fm_expf(-t * 14.0f / dk);
        if (t > 0.4f * dk)
            v->on = 0;
        break;
    default:                                            /* the zap: a falling sine */
        f = 2400.0f * fm_expf(-t * 40.0f) + 80.0f;
        v->ph1 += f * pm / SR;
        if (v->ph1 >= 1.0f)
            v->ph1 -= 1.0f;
        s = fm_sin_pi(v->ph1 * 2.0f - 1.0f) * fm_expf(-t * 12.0f / dk);
        if (t > 0.4f * dk)
            v->on = 0;
        break;
    }
    v->t++;
    return s;
}

static float voice_sample(voice_t *v)
{
    float s, f, hp;
    if (v->drum != 0xFFu)
        return drum_sample(v);
    switch (v->stage) {                                 /* the envelope */
    case 0:
        v->env += v->a_inc;
        if (v->env >= 1.0f) {
            v->env = 1.0f;
            v->stage = 1;
        }
        break;
    case 1:
        v->env = v->s_lvl + (v->env - v->s_lvl) * v->d_coef;
        if (!v->gate)
            v->stage = 3;
        break;
    case 2: break;
    default:
        v->env *= v->r_coef;
        if (v->env < 0.0005f)
            v->on = 0;
        break;
    }
    v->inc1 += (v->inc_target - v->inc1) * v->glide_k;
    {
        float inc = v->inc1 * screw_mul;
        v->ph1 += inc;
        if (v->ph1 >= 1.0f)
            v->ph1 -= 1.0f;
        s = osc(v, v->ph1, inc);
    }
    if (v->wave != WV_NOISE && v->wave != WV_SINE) {   /* the second oscillator, detuned */
        float i2 = v->inc1 * screw_mul * v->inc2;
        v->ph2 += i2;
        if (v->ph2 >= 1.0f)
            v->ph2 -= 1.0f;
        s = 0.6f * (s + (v->wave == WV_SUPER ? 1.0f : 0.6f) * osc(v, v->ph2, i2));
    }
    /* the filter: cutoff 30 Hz .. 12 kHz, the envelope opening it */
    f = v->cut + v->envmod * v->env * (1.0f - v->cut);
    f = 30.0f * fm_exp2f(f * 8.6f);
    f = 2.0f * fm_sin_pi(fm_minf(f / SR, 0.16f)) ;
    hp = s - v->lp - (1.0f - v->res * 0.97f) * 2.0f * v->bp;
    v->bp += f * hp;
    v->lp += f * v->bp;
    v->bp = fm_flush(v->bp);
    v->lp = fm_flush(v->lp);
    return v->lp * v->env;
}

/* n stereo frames from frame off of lr, added to what is there (the caller clears it) */
static void synth_render(int32_t *lr, uint32_t off, uint32_t n, float gain)
{
    uint32_t i, k;
    for (i = off; i < off + n; i++) {
        float mix = 0.0f, send = 0.0f, e, out;
        for (k = 0; k < NVOICE; k++) {
            voice_t *v = &V[k];
            float s;
            if (!v->on)
                continue;
            s = voice_sample(v) * v->vel * v->lvl;
            mix += s;
            send += s * v->echo;
        }
        /* the echo: a darkening tape-ish delay */
        {
            uint32_t d = (uint32_t)(echo_time * SR);
            if (d >= ECHO_N)
                d = ECHO_N - 1u;
            e = (float)echo_buf[(echo_w - d) & (ECHO_N - 1u)] * (1.0f / 16384.0f);
            echo_lp += 0.35f * (e - echo_lp);
            echo_buf[echo_w & (ECHO_N - 1u)] = (int16_t)(fm_clampf(send + echo_lp * echo_fb, -1.99f, 1.99f) * 16384.0f);
            echo_w++;
        }
        out = fm_tanhf((mix + echo_lp) * 0.7f) * gain;
        lr[2 * i] += (int32_t)out;
        lr[2 * i + 1] += (int32_t)out;
    }
}

static int voices_busy(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += V[i].on;
    return (int)n;
}

static void patch_default(patch_t *p, uint32_t type, uint32_t chan)
{
    p->type = (uint8_t)type;
    p->wave = WV_SAW;
    p->cutoff = 70;
    p->reso = 40;
    p->envmod = 50;
    p->attack = 0;
    p->decay = 60;
    p->sustain = 60;
    p->release = 50;
    p->level = 100;
    p->detune = 20;
    p->echo = 0;
    p->chan = (uint8_t)chan;
    p->glide = 0;
    p->octave = 0;
    p->cc = 74;
    if (type == PT_DRUM) {
        p->cutoff = 64;
        p->decay = 64;
        p->level = 110;
    }
}
