#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "jni_resloader.h"
#include "jni_media.h"
#include "input.h"
#include "audio.h"

#include <psp2/kernel/threadmgr.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

// Vita panel resolution. Asphalt5Renderer.onSurfaceCreated() passes the
// display metrics straight through to appInit(), so these are the values the
// engine lays its viewport and UI out against.
// The engine is told 800x480 (not the Vita's real 960x544) -- see
// gl_init()/gl_swap() in utils/glutil.c. Its menu/UI layout apparently
// assumes/requires this reference resolution (reporting the real 960x544
// here visibly broke menu layout on hardware); the actual performance win
// comes from rendering into an offscreen 800x480 FBO (fewer pixels than
// native) and upscale-blitting once per frame, not from this number itself.
#define SCREEN_W 800
#define SCREEN_H 480

/*
 * Asphalt5.nativeInit()'s second argument ends up in the engine's `mbUsePVRT`
 * global, selecting PVRTC-compressed texture assets. Java sets it to 1 except
 * on a handful of blacklisted Adreno/Snapdragon devices; the Vita's SGX543 is
 * a PowerVR part and vitaGL exposes GL_COMPRESSED_*_PVRTC_*, so we take the
 * default path.
 */
#define USE_PVRT 1

// Asphalt5.m_bEnableKeyboard -- set from the Android hardware keyboard config.
// The Vita has no physical keyboard, so the engine's on-screen path is used.
#define ENABLE_KEYBOARD 0

// Asphalt5.mCurrentLang, as returned by getLanguage(); 0 is English.
#define CURRENT_LANG 0

/*
 * The engine caches its own JNIEnv* in the `mEnv` global and every other
 * native entry point dereferences that cache rather than the env it is handed.
 * On Android, Asphalt5Renderer.onSurfaceCreated() primes it by calling
 * nativeGetJNIEnv() before anything else, so we have to do the same or the
 * first nativeInit() faults on `ldr r3, [r2]` with r2 == NULL.
 */
typedef void (* fn_getjnienv)(void *env, void *thiz);

// Asphalt5.nativeInit(int, int) -- static, so the second parameter is a jclass.
typedef void (* fn_app_init)(void *env, void *clazz, int unused, int use_pvrt);

// GLResLoader.nativeInit(int) / GLMediaPlayer.nativeInit(int) -- both static.
typedef void (* fn_sub_init)(void *env, void *clazz, int arg);

// Asphalt5Renderer.nativeInit(int, int, int, int, int) -- an instance method.
typedef void (* fn_renderer_init)(void *env, void *thiz, int enable_keyboard,
                                 int one, int width, int height, int lang);

// Asphalt5Renderer.nativeRender() -- static.
typedef void (* fn_render)(void *env, void *clazz);

// Asphalt5.nativeTouchPressed/Moved/Released(int x, int y, int pointerId) and
// nativeSetOnKeyDown/Up(int keyCode) -- all static, all optional (input just
// won't work if any is missing, it's not fatal to boot).

/**
 * Resolve a native entry point, logging the outcome.
 *
 * Every one of these is required for boot, so a missing symbol is reported
 * rather than silently skipped -- an `if (fn)` guard would just move the crash
 * somewhere less obvious.
 */
static void * resolve(const char * name) {
    void * fn = (void *) so_symbol(&so_mod, name);
    if (!fn)
        l_error("Symbol not found: %s", name);
    else
        // Logged at info level on purpose: these addresses are what turns a PC
        // in a future .psp2dmp back into a libasphalt5.so offset.
        l_info("Resolved %s -> %p", name, fn);
    return fn;
}

int main() {
    soloader_init_all();

    // Asphalt5Renderer.nativeInit() calls importGLInit() and appInit() itself,
    // so GXM has to be live before we get there. On Android this is implied by
    // onSurfaceCreated() only running once EGL has a surface.
    gl_init();
    l_success("OpenGL initialized.");

    audio_init();

    fn_getjnienv     nativeGetJNIEnv       = resolve("Java_com_gameloft_android_GAND_GloftA5HD_Asphalt5Renderer_nativeGetJNIEnv");
    fn_sub_init      GLResLoader_init      = resolve("Java_com_gameloft_android_GAND_GloftA5HD_GLResLoader_nativeInit");
    fn_sub_init      GLMediaPlayer_init    = resolve("Java_com_gameloft_android_GAND_GloftA5HD_GLMediaPlayer_nativeInit");
    fn_app_init      Asphalt5_nativeInit   = resolve("Java_com_gameloft_android_GAND_GloftA5HD_Asphalt5_nativeInit");
    fn_renderer_init Renderer_nativeInit   = resolve("Java_com_gameloft_android_GAND_GloftA5HD_Asphalt5Renderer_nativeInit");
    fn_render        Renderer_nativeRender = resolve("Java_com_gameloft_android_GAND_GloftA5HD_Asphalt5Renderer_nativeRender");
    fn_touch_evt     TouchPressed          = (fn_touch_evt) resolve("Java_com_gameloft_android_GAND_GloftA5HD_Asphalt5_nativeTouchPressed");
    fn_touch_evt     TouchMoved            = (fn_touch_evt) resolve("Java_com_gameloft_android_GAND_GloftA5HD_Asphalt5_nativeTouchMoved");
    fn_touch_evt     TouchReleased         = (fn_touch_evt) resolve("Java_com_gameloft_android_GAND_GloftA5HD_Asphalt5_nativeTouchReleased");
    fn_key_evt       SetOnKeyDown          = (fn_key_evt) resolve("Java_com_gameloft_android_GAND_GloftA5HD_Asphalt5_nativeSetOnKeyDown");
    fn_key_evt       SetOnKeyUp            = (fn_key_evt) resolve("Java_com_gameloft_android_GAND_GloftA5HD_Asphalt5_nativeSetOnKeyUp");

    if (!nativeGetJNIEnv || !Asphalt5_nativeInit || !Renderer_nativeInit
        || !Renderer_nativeRender) {
        l_fatal("Required native entry points are missing.");
        return -1;
    }

    input_init(TouchPressed, TouchMoved, TouchReleased, SetOnKeyDown, SetOnKeyUp);

    /*
     * OS_SCREEN_W gets set to whatever width Game::Game() receives
     * (`OS_SCREEN_W = <width param>`), but OS_SCREEN_H is never written by
     * ANY code in the .so -- confirmed by grepping the whole decompiled
     * source for an assignment to it and finding none; it stays at its
     * compiled-in .data default (480) forever. That happens to already match
     * SCREEN_H (800x480 is the resolution reported to the engine, see the
     * `#define`s above), but poke it explicitly anyway rather than rely on
     * that coincidence surviving if SCREEN_H ever changes -- Game::Game()/
     * InitGL()/the first viewport setup all happen synchronously inside the
     * upcoming Renderer_nativeInit() call.
     */
    int * os_screen_h = (int *) so_symbol(&so_mod, "OS_SCREEN_H");
    if (os_screen_h)
        *os_screen_h = SCREEN_H;
    else
        l_warn("OS_SCREEN_H symbol not found, screen may not fill the display.");

    /*
     * From here on the order mirrors Asphalt5Renderer.onSurfaceCreated()
     * exactly. It is load-bearing: steps 2-5 all read the `mEnv` primed by
     * step 1, and each registers its own set of jmethodIDs against FalsoJNI.
     */

    // 1. Prime the engine's cached JNIEnv*. This is the fix for the data abort
    //    at libasphalt5.so+0x5cbf0 (mEnv == NULL).
    nativeGetJNIEnv(&jni, &jni);
    l_success("mEnv primed via nativeGetJNIEnv.");

    // 2. GLResLoader.init() -- resource-loader callbacks.
    if (GLResLoader_init) {
        GLResLoader_init(&jni, &jni, 0);
        l_success("GLResLoader initialized.");
    }

    // 3. GLMediaPlayer.init() -- audio/video callbacks.
    if (GLMediaPlayer_init) {
        GLMediaPlayer_init(&jni, &jni, 0);
        l_success("GLMediaPlayer initialized.");
    }

    // 4. Asphalt5.nativeInit(0, pvrt) -- registers the 13 static Asphalt5
    //    methods (Exit, IsDemo, NotifyTrophy, ...) and sets mbUsePVRT.
    Asphalt5_nativeInit(&jni, &jni, 0, USE_PVRT);
    l_success("Asphalt5.nativeInit done (mbUsePVRT=%d).", USE_PVRT);

    // 5. Asphalt5Renderer.nativeInit(...) -- importGLInit() + appInit(w,h,lang).
    //    The `1` is a literal in the Java caller, not a variable.
    Renderer_nativeInit(&jni, &jni, ENABLE_KEYBOARD, 1,
                        SCREEN_W, SCREEN_H, CURRENT_LANG);
    l_success("Asphalt5Renderer.nativeInit done (%dx%d).", SCREEN_W, SCREEN_H);

    l_info("Entering render loop. Log: %s", log_current_path());

    while (1) {
        input_poll(&jni, &jni);
        Renderer_nativeRender(&jni, &jni);
        gl_swap();
        media_pump();
    }

    resloader_shutdown();
    log_shutdown();
    sceKernelExitDeleteThread(0);
}
