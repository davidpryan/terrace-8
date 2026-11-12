/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Ryan */
/* TERRACE-8: the unity build, after fm1-base's platform.
 *   core/   shared with the other five firmwares      seq.c  the stages and lines      ui.c  controls and screen */
#include "fastmath.h"
#include "core/util.c"
#include "core/synth.c"
#include "core/clock.c"
#include "core/slots.c"
#include "core/kit.c"
#include "seq.c"
#include "ui.c"

void app_init(void)
{
    uint32_t k;
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC || set.slot >= NSLOTS) {
        set.magic = SET_MAGIC;
        set.slot = 0;
        set.ble_off = 0;
    }
    if (!slot_load(set.slot, PROJ_MAGIC, &P, sizeof P))
        project_new();
    project_apply();
    R.seed = 0x7E44;
    for (k = 0; k < NKEYS; k++)
        U.key_note[k] = -1;
    U.held = -1;
    U.page = PG_LINES;
    K.card = NB;
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    clock_render(lr, n, (float)master_q12 * (1.0f / 4096.0f) * 7000000.0f);
}

void app_panic(void)
{
    T.playing = 0;
    voices_silence();
}

void app_ui_init(void)
{
    touch_on(1);
    lcd_fill(0, 0, 240, 240, K_BG);
    K.redraw = 1;
}

void app_input_only(void) { input(); }

void app_frame(void)
{
    static uint32_t last;
    uint32_t now = plat_ms(), i;
    input();
    if (ann_on() && ann_where()) {
        char t[48];
        s_cpy(t, APP_NAME ". ");
        s_cat(t, U.page == PG_LINES ? "STAGES" : U.page == PG_SOUND ? "SOUND" : "SETUP");
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(now);
    if (U.save_wait && !T.playing) {
        U.save_wait = 0;
        project_save_now();
    }
    if (K.msg_until && now >= K.msg_until) {
        K.msg_until = 0;
        K.redraw = 1;
    }
    if (T.playing && now - last >= 40u)
        K.redraw = 1;
    for (i = 0; i < 16u; i++)
        if (heard[i])
            heard[i]--;
    lights();
    if (K.redraw) {
        K.redraw = 0;
        last = now;
        draw();
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "playing")) *got = T.playing;
    else if (!strcmp(what, "page")) *got = U.page;
    else if (!strcmp(what, "line")) *got = U.line;
    else if (!strcmp(what, "stage")) *got = U.stage;
    else if (!strcmp(what, "deg")) *got = stg()->deg;
    else if (!strcmp(what, "pulses")) *got = stg()->pulses;
    else if (!strcmp(what, "gate")) *got = stg()->gate;
    else if (!strcmp(what, "slide")) *got = stg()->slide;
    else if (!strcmp(what, "skip")) *got = stg()->skip;
    else if (!strcmp(what, "ratchet")) *got = stg()->ratchet;
    else if (!strcmp(what, "nstages")) *got = ln()->n;
    else if (!strcmp(what, "dir")) *got = ln()->dir;
    else if (!strcmp(what, "playstage")) *got = R.l[U.line].stage;
    else if (!strcmp(what, "acc")) *got = R.l[U.line].acc;
    else if (!strcmp(what, "rec")) *got = U.rec;
    else if (!strcmp(what, "card")) *got = K.card == NB ? -1 : K.card;
    else if (!strcmp(what, "dirty")) *got = U.dirty;
    else return 0;
    return 1;
}
#endif
