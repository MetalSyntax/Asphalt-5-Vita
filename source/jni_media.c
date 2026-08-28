/*
 * Copyright (C) 2026 Asphalt-5-Vita contributors
 */

#include "jni_media.h"
#include "utils/logger.h"
#include <so_util/so_util.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

extern so_module so_mod;

#define GAME_MOVIE_BUSY_OFFSET 0x1D72

static uintptr_t * s_game_slot = NULL;
static bool         s_movie_active = false;

static const char * jstring_to_cstr(jobject jstr) {
    if (!jstr)
        return NULL;
    JavaString * js = (JavaString *) jstr;
    if (!js->utf8 || !js->utf8->array)
        return NULL;
    return (const char *) js->utf8->array;
}

#include "video.h"

void impl_GLMediaPlayer_loadMovie(jmethodID id, va_list args) {
    const char *name = jstring_to_cstr((jobject) va_arg(args, jobject));

    if (!s_game_slot)
        s_game_slot = (uintptr_t *) so_symbol(&so_mod, "g_pMainGameClass");

    if (name && strlen(name) > 0) {
        l_info("[movie] GLMediaPlayer.loadMovie(%s): starting playback via software decode", name);
        video_play(name);
        l_info("[movie] GLMediaPlayer.loadMovie(%s): video_play returned", name);
    } else {
        l_warn("[movie] GLMediaPlayer.loadMovie: null/empty name");
    }

    // Tell media_pump to clear the flag on the NEXT frame, 
    // AFTER GS_TrailerMovie::Create() finishes setting it to 1!
    s_movie_active = true;
}

void media_pump(void) {
    if (s_movie_active && s_game_slot && *s_game_slot) {
        *(uint8_t *)(*s_game_slot + GAME_MOVIE_BUSY_OFFSET) = 0;
        s_movie_active = false;
        l_info("[movie] media_pump: video flag cleared, game can proceed.");
    }
}
