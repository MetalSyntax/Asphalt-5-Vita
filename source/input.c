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

#include <so_util/so_util.h>
extern so_module so_mod;

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

// Marker for slots owned by poll_keys() fake touches -- never a real
// SceTouchReport id. poll_touch() skips these (it must not track or
// release them); only fake_touch_set/release manage them.
#define FAKE_VITA_ID 0xFF

// Fake-touch indices (buttons mapped to synthetic taps).
#define FAKE_IDX_LEFT  0
#define FAKE_IDX_RIGHT 1
#define FAKE_IDX_CROSS 2
#define FAKE_IDX_SQUARE 3
#define FAKE_IDX_START 4
#define FAKE_IDX_MENU_TAP 5
#define FAKE_COUNT 6

typedef struct {
    bool active;
    uint8_t vita_id;
    int x, y;
} touch_slot;

static touch_slot   s_slots[MAX_TOUCH_SLOTS];
static fn_touch_evt  s_pressed, s_moved, s_released;
static fn_key_evt    s_key_down, s_key_up;
static bool          s_circle_was_down = false;
// SELECT (unused in-race) toggles the on-screen brake/nitro icons between
// ~1% opacity (default -- physical buttons drive them) and fully visible.
static bool          s_select_was_down = false;
static bool          s_touch_buttons_visible = false;

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
            if (s_slots[s].active && s_slots[s].vita_id == FAKE_VITA_ID)
                continue; // owned by fake_touch_set(), never match a finger
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
            l_info("input: touch press (%d,%d) slot %d", x, y, slot);
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
        if (s_slots[s].active && !seen[s] && s_slots[s].vita_id != FAKE_VITA_ID) {
            s_slots[s].active = false;
            l_info("input: touch release (%d,%d) slot %d", s_slots[s].x, s_slots[s].y, s);
            if (s_released)
                s_released(env, clazz, s_slots[s].x, s_slots[s].y, s);
        }
    }
}

typedef enum {
    APP_STATE_UNKNOWN,
    APP_STATE_MENU,
    APP_STATE_TITLE,
    APP_STATE_INGAME
} app_state_t;

// out_vtable_base (may be NULL) gets the raw vtable pointer of the current
// top-of-stack gxGameState -- diagnostic only, so a log can be matched
// against "vtable for GS_XXX" in libasphalt5_symbols_by_addr.txt to tell
// APART the many real engine screens (GS_IngameMenu, the CPanel confirm
// dialog, GS_MainMenu, GS_LoadMainMenu, GS_EndRaceScreen...) that all
// collapse into the single APP_STATE_MENU bucket below.
static app_state_t get_current_app_state(void **out_vtable_base) {
    static void *g_pMainGameClass = NULL;
    static void *vtable_GS_Run = NULL;
    static void *vtable_GS_Splash = NULL;
    static void *vtable_GS_GLLogo = NULL;
    static void *vtable_GS_TrailerMovie = NULL;

    if (out_vtable_base) *out_vtable_base = NULL;

    if (!g_pMainGameClass) {
        g_pMainGameClass = (void*)so_symbol(&so_mod, "g_pMainGameClass");
        vtable_GS_Run = (void*)so_symbol(&so_mod, "_ZTV6GS_Run");
        vtable_GS_Splash = (void*)so_symbol(&so_mod, "_ZTV9GS_Splash");
        vtable_GS_GLLogo = (void*)so_symbol(&so_mod, "_ZTV9GS_GLLogo");
        vtable_GS_TrailerMovie = (void*)so_symbol(&so_mod, "_ZTV15GS_TrailerMovie");
    }

    if (!g_pMainGameClass || !vtable_GS_Run)
        return APP_STATE_UNKNOWN;

    void *game = *(void**)g_pMainGameClass;
    if (!game) return APP_STATE_UNKNOWN;

    int top = *(int*)((uintptr_t)game + 0x1D38);
    if (top < 0 || top > 10) return APP_STATE_UNKNOWN; // Sanity check

    void *state = *(void**)((uintptr_t)game + 0x1D3C + top * 4);
    if (!state) return APP_STATE_UNKNOWN;

    void *vtable = *(void**)state;
    // In the Itanium C++ ABI (used by ARM), an object's vptr points to the first function
    // in the vtable, which is 8 bytes after the vtable symbol (skipping offset-to-top and typeinfo).
    void *vtable_base = (void*)((uintptr_t)vtable - 8);
    if (out_vtable_base) *out_vtable_base = vtable_base;

    if (vtable_base == vtable_GS_Run)
        return APP_STATE_INGAME;
    if (vtable_base == vtable_GS_Splash || vtable_base == vtable_GS_GLLogo || vtable_base == vtable_GS_TrailerMovie)
        return APP_STATE_TITLE;

    return APP_STATE_MENU;
}

static int s_fake_slot[FAKE_COUNT] = { -1, -1, -1, -1, -1, -1 };
static bool s_fake_down[FAKE_COUNT] = { false, false, false, false, false, false };
static int s_fake_x[FAKE_COUNT] = { 100, 700, 720, 50, 50, 400 };
static int s_fake_y[FAKE_COUNT] = { 240, 240, 400, 430, 50, 240 };

// Track fake touches to emit pressed/released
static bool s_fake_left_down = false;
static bool s_fake_right_down = false;
static bool s_fake_cross_down = false;
static bool s_fake_square_down = false;
static bool s_fake_start_down = false;
static bool s_fake_menu_tap_down = false;
static app_state_t s_prev_state = APP_STATE_UNKNOWN;

static void fake_touch_release(void *env, void *clazz, int idx) {
    if (idx < 0 || idx >= FAKE_COUNT)
        return;
    int slot = s_fake_slot[idx];
    if (slot >= 0 && slot < MAX_TOUCH_SLOTS) {
        if (s_released)
            s_released(env, clazz, s_fake_x[idx], s_fake_y[idx], slot);
        s_slots[slot].active = false;
    }
    s_fake_slot[idx] = -1;
    s_fake_down[idx] = false;
}

// Nitro (CROSS) is a one-shot burst the engine consumes on the very touch
// event that lands inside its button rect -- steering (LEFT/RIGHT) is a
// sustained hold. With only MAX_TOUCH_SLOTS=2 shared between up to 5 fake
// buttons, holding a turn (1 slot) plus the brake (SQUARE, the 2nd slot) --
// the exact moment a player wants to nitro out of a corner -- left CROSS
// with no free slot: fake_touch_set() silently dropped the press and the
// player had to mash it until a slot happened to free up. Evict whichever
// non-steering action (SQUARE/START) currently holds a slot so CROSS always
// gets one immediately; that held button just re-claims a slot the next
// frame one is free, same as if it had been briefly released.
static void fake_touch_evict_for(void *env, void *clazz, int idx) {
    for (int s = 0; s < MAX_TOUCH_SLOTS; s++)
        if (!s_slots[s].active)
            return; // a slot is already free, nothing to evict
    for (int other = 0; other < FAKE_COUNT; other++) {
        if (other == idx || other == FAKE_IDX_LEFT || other == FAKE_IDX_RIGHT)
            continue;
        if (s_fake_slot[other] >= 0) {
            l_info("input: nitro preempting fake idx %d for a free touch slot", other);
            fake_touch_release(env, clazz, other);
            return;
        }
    }
}

static void fake_touch_set(void *env, void *clazz, int idx, bool down, int x, int y) {
    if (idx < 0 || idx >= FAKE_COUNT)
        return;
    s_fake_x[idx] = x;
    s_fake_y[idx] = y;
    if (down == s_fake_down[idx] && (down == false || s_fake_slot[idx] >= 0))
        return;
    if (!down) {
        fake_touch_release(env, clazz, idx);
        return;
    }
    if (idx == FAKE_IDX_CROSS && s_fake_slot[idx] < 0)
        fake_touch_evict_for(env, clazz, idx);
    // Press: claim a free real slot so we can never collide with a live
    // finger tracked by poll_touch().
    if (s_fake_slot[idx] < 0) {
        for (int s = 0; s < MAX_TOUCH_SLOTS; s++) {
            if (!s_slots[s].active) {
                s_fake_slot[idx] = s;
                s_slots[s].active = true;
                s_slots[s].vita_id = FAKE_VITA_ID;
                s_slots[s].x = x;
                s_slots[s].y = y;
                break;
            }
        }
        if (s_fake_slot[idx] < 0)
            return; // both slots busy with real fingers -- retry next frame
    }
    s_fake_down[idx] = true;
    if (s_pressed)
        s_pressed(env, clazz, x, y, s_fake_slot[idx]);
}

static void input_release_all_fake(void *env, void *clazz) {
    for (int i = 0; i < FAKE_COUNT; i++) {
        if (s_fake_down[i] || s_fake_slot[i] >= 0)
            fake_touch_release(env, clazz, i);
    }
    // Key-emulating flags share storage with the touch flags below; make
    // sure a held DPAD key can't stick either.
    if (s_fake_left_down && s_key_up) s_key_up(env, clazz, 19);
    if (s_fake_right_down && s_key_up) s_key_up(env, clazz, 20);
    if (s_fake_cross_down && s_key_up) s_key_up(env, clazz, 23);
    s_fake_left_down = s_fake_right_down = s_fake_cross_down = false;
    s_fake_square_down = s_fake_start_down = s_fake_menu_tap_down = false;
}

static void *s_prev_vtable = (void*)-1; // -1: never logged yet, distinct from a real NULL/unknown

static void poll_keys(void * env, void * clazz) {
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0)
        return;

    void *vtable_base = NULL;
    app_state_t state = get_current_app_state(&vtable_base);
    if (vtable_base != s_prev_vtable) {
        l_info("input: game state vtable now %p (bucket %d)", vtable_base, (int) state);
        s_prev_vtable = vtable_base;
    }
    if (state != s_prev_state) {
        // INGAME<->MENU<->TITLE reuse the same s_fake_* flags with different
        // meanings; a button held across the transition would otherwise leave
        // a touch pressed (or key down) that is never released, and the engine
        // treats that slot as a stuck finger -- post-race screens then see
        // every fresh tap as a move of the ghost finger and never advance.
        l_info("input: state %d -> %d", (int) s_prev_state, (int) state);
        input_release_all_fake(env, clazz);
        s_prev_state = state;
    }

    if (state == APP_STATE_INGAME) {
        // IN-GAME Mapping -- every synthetic tap goes through the shared
        // slot allocator (fake_touch_set), never a hardcoded slot.
        // Left analog stick steers too (deadzone matches the menu's pad.ly
        // threshold below) -- the D-pad/L/R mapping alone left the stick
        // completely unused for driving.
        bool left_down = (pad.buttons & SCE_CTRL_LEFT) != 0 || (pad.buttons & SCE_CTRL_LTRIGGER) != 0 || pad.lx < 64;
        bool right_down = (pad.buttons & SCE_CTRL_RIGHT) != 0 || (pad.buttons & SCE_CTRL_RTRIGGER) != 0 || pad.lx > 192;
        bool cross_down = (pad.buttons & SCE_CTRL_CROSS) != 0;
        bool square_down = (pad.buttons & SCE_CTRL_SQUARE) != 0;
        bool start_down = (pad.buttons & SCE_CTRL_START) != 0;

        fake_touch_set(env, clazz, FAKE_IDX_LEFT, left_down, 100, 240);
        fake_touch_set(env, clazz, FAKE_IDX_RIGHT, right_down, 700, 240);
        // (715,380): recentered from the original (720,400) guess using the
        // real touchscreen taps logged in asphalt5_072.log while the player
        // mashed nitro by hand -- they clustered at x=700-730,y=368-391
        // (center ~715,380), all forwarded cleanly (press+release, no
        // dropped slot) yet only registered as nitro every few tries. That
        // points at the button's hitbox being narrower than our guessed
        // center, not at a forwarding bug -- nudge CROSS's synthetic tap to
        // the empirically-observed hot zone so the physical button doesn't
        // inherit the same near-miss.
        fake_touch_set(env, clazz, FAKE_IDX_CROSS, cross_down, 715, 380);
        fake_touch_set(env, clazz, FAKE_IDX_SQUARE, square_down, 50, 430);
        fake_touch_set(env, clazz, FAKE_IDX_START, start_down, 50, 50);
        bool select_down = (pad.buttons & SCE_CTRL_SELECT) != 0;
        if (select_down && !s_select_was_down) {
            s_touch_buttons_visible = !s_touch_buttons_visible;
            l_info("input: on-screen brake/nitro buttons %s", s_touch_buttons_visible ? "shown" : "dimmed");
        }
        s_select_was_down = select_down;
        s_fake_left_down = left_down;
        s_fake_right_down = right_down;
        s_fake_cross_down = cross_down;
        s_fake_square_down = square_down;
        s_fake_start_down = start_down;

    } else if (state == APP_STATE_TITLE) {
        bool any_down = (pad.buttons & (SCE_CTRL_CROSS | SCE_CTRL_SQUARE | SCE_CTRL_TRIANGLE | SCE_CTRL_START)) != 0;
        fake_touch_set(env, clazz, FAKE_IDX_START, any_down, 400, 240);
        s_fake_start_down = any_down;

    } else {
        // MENU Mapping (APP_STATE_MENU -- includes post-race GS_EndRaceScreen /
        // GS_RaceSummary, whose phase-1 gate only watches s_mouseCount, i.e. a
        // fresh touch; key 23 alone can never advance it). So CROSS sends BOTH
        // a center tap (advances the gate) and DPAD_CENTER (for later nav).
        bool up_down = (pad.buttons & SCE_CTRL_UP) != 0 || pad.ly < 64;
        bool down_down = (pad.buttons & SCE_CTRL_DOWN) != 0 || pad.ly > 192;
        bool cross_down = (pad.buttons & SCE_CTRL_CROSS) != 0;

        #define DISPATCH_KEY(btn_state, is_down, keycode) \
            if (is_down != btn_state) { \
                btn_state = is_down; \
                if (is_down && s_key_down) s_key_down(env, clazz, keycode); \
                else if (!is_down && s_key_up) s_key_up(env, clazz, keycode); \
            }

        DISPATCH_KEY(s_fake_left_down, up_down, 19); // KEYCODE_DPAD_UP
        DISPATCH_KEY(s_fake_right_down, down_down, 20); // KEYCODE_DPAD_DOWN
        DISPATCH_KEY(s_fake_cross_down, cross_down, 23); // KEYCODE_DPAD_CENTER
        // Post-race screens ignore keys -- CROSS also taps center.
        fake_touch_set(env, clazz, FAKE_IDX_MENU_TAP, cross_down, 400, 240);
        s_fake_menu_tap_down = cross_down;

        #undef DISPATCH_KEY
    }

    // BACK button (Circle) for menu navigation only -- NOT in-game. Android's
    // BACK also opens the pause menu from GS_Run, which duplicated exactly
    // what START (FAKE_IDX_START, tapping the pause icon) already does; the
    // user asked for that overlap gone since START already covers it.
    bool circle_down = state != APP_STATE_INGAME && (pad.buttons & SCE_CTRL_CIRCLE) != 0;
    if (circle_down != s_circle_was_down) {
        s_circle_was_down = circle_down;
        if (circle_down && s_key_down) s_key_down(env, clazz, 4 /* KEYCODE_BACK */);
        else if (!circle_down && s_key_up) s_key_up(env, clazz, 4 /* KEYCODE_BACK */);
    }
}

void input_poll(void * env, void * clazz) {
    poll_touch(env, clazz);
    poll_keys(env, clazz);
}

int input_in_race(void) {
    return s_prev_state == APP_STATE_INGAME;
}

int input_touch_buttons_visible(void) {
    return s_touch_buttons_visible;
}
