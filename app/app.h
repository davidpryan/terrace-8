/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Ryan */
/* TERRACE-8: a stage sequencer for the M-VAVE FM-1 on fm1-base. The app's half of the contract with the platform. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "TERRACE-8"
#define APP_SPLASH_RGB RGB(240, 170, 90)
#define APP_BLOCK 128u                   /* frames per render: 2.9 ms (the sequencer runs in it, sample-exact) */
#define APP_CANVAS_PX (240u * 200u)      /* the page band */
#define APP_TOUCH                        /* velocity from the keys */
#define APP_BTDEV
#define APP_BLE_AT_BOOT (!terrace_ble_off())
int terrace_ble_off(void);

enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_BLE, OBJ_NOBJ };
#define APP_STORE_MAP {PLAT_GLOB, 0xDC000u, 0xDE000u}

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
