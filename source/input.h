/*
 * Copyright (C) 2026 Asphalt-5-Vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  input.h
 * @brief Drives the engine's touch/key native entry points from the Vita's
 *        front touch panel and physical buttons.
 *
 * `libasphalt5.so` never reads input itself: on Android, `Asphalt5`'s
 * `onTouchEvent`/`onKeyDown`/`onKeyUp` (driven by the framework) call five
 * exported native entry points --
 *
 *   Java_..._Asphalt5_nativeTouchPressed(x, y, pointerId)
 *   Java_..._Asphalt5_nativeTouchMoved(x, y, pointerId)
 *   Java_..._Asphalt5_nativeTouchReleased(x, y, pointerId)
 *   Java_..._Asphalt5_nativeSetOnKeyDown(keyCode)
 *   Java_..._Asphalt5_nativeSetOnKeyUp(keyCode)
 *
 * -- confirmed against `Asphalt5.java`/`TouchManager_multi.java` (jadx) and
 * `libasphalt5.so`'s dynamic symbol table (all five present, all static).
 * There's no Android framework here, so `input_poll()` polls the Vita's
 * touch panel and pad directly and calls these the same way `main.c` drives
 * `nativeInit`/`nativeRender`.
 */

#ifndef SOLOADER_INPUT_H
#define SOLOADER_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (* fn_touch_evt)(void * env, void * clazz, int x, int y, int id);
typedef void (* fn_key_evt)(void * env, void * clazz, int keycode);

/** Store the resolved native entry points. Any of these may be NULL. */
void input_init(fn_touch_evt pressed, fn_touch_evt moved, fn_touch_evt released,
                 fn_key_evt key_down, fn_key_evt key_up);

/** Poll touch + pad and fire the callbacks given to `input_init()`. Call once per frame. */
void input_poll(void * env, void * clazz);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_INPUT_H
