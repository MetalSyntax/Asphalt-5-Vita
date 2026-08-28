/*
 * Copyright (C) 2026 Asphalt-5-Vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  jni_media.h
 * @brief Real implementation of `GLMediaPlayer.loadMovie(String)`.
 *
 * On Android this launches a separate `MyVideoView` Activity that plays the
 * intro/trailer `.mp4` full-screen and, once it finishes, lets the engine's
 * "movie busy" flag (`Game` object, offset `0x1D72`) get cleared so
 * `GS_TrailerMovie::Update()` moves on to the main menu.
 *
 * There is no video decoder wired up on the Vita, so instead of launching
 * anything this arms a wait: the trailer screen stays blank until the player
 * presses X, polled directly via `sceCtrlPeekBufferPositive` from
 * `media_pump()` (called once per frame from the render loop). Once pressed,
 * the flag is cleared and `GS_TrailerMovie::Update()` moves on like it would
 * after real playback finished.
 *
 * A first attempt used a `SceMsgDialog` ("press X/OK to continue") instead of
 * raw button polling, so the skip would come with an on-screen prompt. It
 * never became visible/responsive on hardware for reasons not pinned down
 * (dialog init timing relative to the still-mid-`appInit()` GXM state was the
 * leading suspect) -- direct `sceCtrl` polling has far fewer moving parts and
 * is what's in place now. Revisit the dialog only if an on-screen prompt is
 * worth another look.
 *
 * @warning Hand-written override wired into `source/java.c`, registered
 *          under BOTH method ids `14` (`Asphalt5.loadMovie`) and `69`
 *          (`GLMediaPlayer.loadMovie`) -- see the comment at id `14` in
 *          `java.c` for why both. If the toolkit regenerates `java.c`,
 *          re-point both back at `impl_GLMediaPlayer_loadMovie` or the
 *          engine hangs on the trailer state again (blank/undrawn screen,
 *          no crash, and no way to get past it).
 */

#ifndef SOLOADER_JNI_MEDIA_H
#define SOLOADER_JNI_MEDIA_H

#include <falso_jni/FalsoJNI_ImplBridge.h>

#ifdef __cplusplus
extern "C" {
#endif

/** `GLMediaPlayer.loadMovie(String)` -- arms the "press X to skip" wait. */
void impl_GLMediaPlayer_loadMovie(jmethodID id, va_list args);

/**
 * Poll for the X press armed by `impl_GLMediaPlayer_loadMovie`. A no-op
 * unless a skip is currently pending. Call once per frame from the render
 * loop.
 */
void media_pump(void);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_JNI_MEDIA_H
