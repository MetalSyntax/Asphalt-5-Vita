/*
 * Copyright (C) 2026 Asphalt-5-Vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "input.h"

#include "utils/logger.h"

#include <psp2/touch.h>
#include <psp2/ctrl.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * `notifyTouchPress`/`notifyTouchMoved`/`notifyTouchReleased` (the internal
 * handlers these native entry points call, confirmed in libasphalt5.so's
 * pseudo-C) index a 2-slot array (`mTouchID`) by pointer id and ASSERT if
 * `id > 1`. The engine only ever expects ids 0/1 -- track at most two
 * concurrent touches and assign them stable small ids ourselves rather than
 * forwarding the Vita's own touch report ids (which are not bounded to 0/1).
 */
#define MAX_TOUCH_SLOTS 2

/*
 * Front touch panel is 1920x1088. The engine is told 800x480 in
 * Renderer_nativeInit() (main.c's SCREEN_W/H -- its menu/UI layout needs
 * exactly that value, confirmed on hardware), and
 * nativeTouchPressed/Moved/Released take coordinates in THAT space, not the
 * real 960x544 the panel/screen physically are. Scale straight from panel
 * pixels to the engine's 800x480 space -- do NOT go through the intermediate
 * 960x544 screen resolution, that would touch-offset everything since the
 * game itself doesn't know the screen is really bigger than 800x480.
 */
#define TOUCH_PANEL_W 1920
#define TOUCH_PANEL_H 1088
#define TOUCH_TARGET_W 800
#define TOUCH_TARGET_H 480

typedef struct {
    bool active;
    uint8_t vita_id;
    int x, y;
} touch_slot;

static touch_slot   s_slots[MAX_TOUCH_SLOTS];
static fn_touch_evt  s_pressed, s_moved, s_released;
static fn_key_evt    s_key_down, s_key_up;
static bool          s_circle_was_down = false;

void input_init(fn_touch_evt pressed, fn_touch_evt moved, fn_touch_evt released,
                 fn_key_evt key_down, fn_key_evt key_up) {
    s_pressed  = pressed;
    s_moved    = moved;
    s_released = released;
    s_key_down = key_down;
    s_key_up   = key_up;
    memset(s_slots, 0, sizeof(s_slots));

    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    if (!pressed || !moved || !released)
        l_warn("input: one or more nativeTouch* entry points missing, touch will not work.");
    if (!key_down || !key_up)
        l_warn("input: nativeSetOnKey* entry points missing, buttons will not work.");
}

static void poll_touch(void * env, void * clazz) {
    SceTouchData touch;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) < 0)
        return;

    bool seen[MAX_TOUCH_SLOTS] = { false };

    for (int i = 0; i < touch.reportNum && i < SCE_TOUCH_MAX_REPORT; i++) {
        int x = (touch.report[i].x * TOUCH_TARGET_W) / TOUCH_PANEL_W;
        int y = (touch.report[i].y * TOUCH_TARGET_H) / TOUCH_PANEL_H;
        uint8_t vid = touch.report[i].id;

        int slot = -1;
        for (int s = 0; s < MAX_TOUCH_SLOTS; s++) {
            if (s_slots[s].active && s_slots[s].vita_id == vid) {
                slot = s;
                break;
            }
        }

        if (slot < 0) {
            for (int s = 0; s < MAX_TOUCH_SLOTS; s++) {
                if (!s_slots[s].active) {
                    slot = s;
                    break;
                }
            }
            if (slot < 0)
                continue; // both slots taken, drop any extra fingers

            s_slots[slot].active  = true;
            s_slots[slot].vita_id = vid;
            s_slots[slot].x       = x;
            s_slots[slot].y       = y;
            seen[slot] = true;
            if (s_pressed)
                s_pressed(env, clazz, x, y, slot);
            continue;
        }

        seen[slot] = true;
        if (s_slots[slot].x != x || s_slots[slot].y != y) {
            s_slots[slot].x = x;
            s_slots[slot].y = y;
            if (s_moved)
                s_moved(env, clazz, x, y, slot);
        }
    }

    for (int s = 0; s < MAX_TOUCH_SLOTS; s++) {
        if (s_slots[s].active && !seen[s]) {
            s_slots[s].active = false;
            if (s_released)
                s_released(env, clazz, s_slots[s].x, s_slots[s].y, s);
        }
    }
}

static void poll_keys(void * env, void * clazz) {
    // KeyEvent.KEYCODE_BACK -- the only keycode libasphalt5.so's
    // notifyKeyPressed/Released path is confirmed to branch on
    // (`param_2 == 4` in several menu/dialog handlers). Circle is the
    // universal Vita "back/cancel" button regardless of region button swap.
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0)
        return;

    bool circle_down = (pad.buttons & SCE_CTRL_CIRCLE) != 0;
    if (circle_down != s_circle_was_down) {
        s_circle_was_down = circle_down;
        if (circle_down) {
            if (s_key_down)
                s_key_down(env, clazz, 4 /* KEYCODE_BACK */);
        } else {
            if (s_key_up)
                s_key_up(env, clazz, 4 /* KEYCODE_BACK */);
        }
    }
}

void input_poll(void * env, void * clazz) {
    poll_touch(env, clazz);
    poll_keys(env, clazz);
}
