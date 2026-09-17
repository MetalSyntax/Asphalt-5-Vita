/*
 * Copyright (C) 2026 Asphalt-5-Vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  jni_lifecycle.h
 * @brief Real implementation of `Asphalt5.Exit()`.
 *
 * On Android this calls back into the framework to finish the `Activity`.
 * The engine calls it itself -- `Game::Exit()` does
 * `CallStaticVoidMethod(env, mClassGLAsphalt5, mMethodExit)` -- from the
 * "Exit" confirmation panel in `GS_IngameMenu::Update()` (`this+0x958==2`,
 * confirmed in the pseudo-C) and the main menu's own Exit item. The
 * auto-generated stub (`stub_Asphalt5_Exit_18`) only logged a debug line, so
 * confirming "Exit" never actually closed the app on Vita -- no crash, no
 * error, just silence.
 *
 * @warning Hand-written override wired into `source/java.c` (id `18`,
 * `Asphalt5.Exit`) -- if the toolkit regenerates `java.c`, re-point it back
 * at `impl_Asphalt5_Exit` or the app becomes unclosable again.
 */

#ifndef SOLOADER_JNI_LIFECYCLE_H
#define SOLOADER_JNI_LIFECYCLE_H

#include <falso_jni/FalsoJNI_ImplBridge.h>

#ifdef __cplusplus
extern "C" {
#endif

/** `Asphalt5.Exit()` -- actually terminates the Vita process. */
void impl_Asphalt5_Exit(jmethodID id, va_list args);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_JNI_LIFECYCLE_H
