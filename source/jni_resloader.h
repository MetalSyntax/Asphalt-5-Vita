/*
 * Copyright (C) 2026 Asphalt-5-Vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  jni_resloader.h
 * @brief Real implementations of the `GLResLoader` resource callbacks.
 *
 * `libasphalt5.so` does not open its data files itself. `GLResLoader_nativeInit`
 * resolves three *static* Java methods and the engine pulls every byte of game
 * data through them:
 *
 *   - `getResourceLength(Ljava/lang/String;)I`   -- size, or 0 if absent
 *   - `getResourceFull(Ljava/lang/String;)[B`    -- whole file
 *   - `getResourceBytes(Ljava/lang/String;II)[B` -- `loadSize` bytes at `offset`
 *
 * On Android these read from `GLMediaPlayer.SOUND_DIR`
 * (`/sdcard/gameloft/games/asphalt5/`), from `res/drawable/res_<name>`
 * "protected" resources, or from the APK's assets. This APK ships no `assets/`
 * and its `res/drawable/` holds only the IGP billing UI, so the sdcard
 * directory is the only real source -- it maps to `RES_PATH` on the Vita
 * (`DATA_PATH` + `data/`, kept separate from `DATA_PATH`'s other tenants:
 * the `.so`, logs/, config.txt, shader cache).
 *
 * The auto-generated stubs returned `0`/`NULL`, which is what made
 * `GamePackageMgr::Init()` walk into a NULL `LZMAFile`. These replace them.
 *
 * @warning These are hand-written overrides wired into `source/java.c`. If the
 *          toolkit regenerates `java.c`, re-point method IDs 75/76/79 back at
 *          `impl_GLResLoader_*` or the game returns to failing at boot.
 */

#ifndef SOLOADER_JNI_RESLOADER_H
#define SOLOADER_JNI_RESLOADER_H

#include <falso_jni/FalsoJNI_ImplBridge.h>

#ifdef __cplusplus
extern "C" {
#endif

/** `GLResLoader.getResourceLength(String)` -- byte size, or `0` if missing. */
jint impl_GLResLoader_getResourceLength(jmethodID id, va_list args);

/** `GLResLoader.getResourceFull(String)` -- whole file, or `NULL` on failure. */
jobject impl_GLResLoader_getResourceFull(jmethodID id, va_list args);

/** `GLResLoader.getResourceBytes(String, int offset, int loadSize)`. */
jobject impl_GLResLoader_getResourceBytes(jmethodID id, va_list args);

/** Close the cached file handle. Call on shutdown; optional. */
void resloader_shutdown(void);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_JNI_RESLOADER_H
