/**
 * @file video.cpp
 * @brief Cutscene video playback via software MPEG-4/AAC decode (FFmpeg).
 *
 * @details The PS Vita's hardware video decoder (`SceVideodec`, used
 * internally by `SceAvPlayer`) only decodes H.264/AVC -- it has no hardware
 * path for MPEG-4 Part 2, which is what every `.mp4` asset this game ships
 * actually is (`mp4v` fourcc, "Simple Profile", confirmed via `ffprobe` on
 * all 7 -- the `_H264` suffix some of the filenames have does NOT reflect
 * their real codec). Confirmed on hardware earlier this session:
 * `SceAvPlayer` opens/demuxes one of these files fine (real file reads
 * happen, the `moov` atom at the end of the file gets found) but the
 * decoder itself never produces a single frame and self-stops after ~1.6s
 * with no error event at all -- see port_progress.md's "Bug #17" section.
 *
 * Re-encoding the game's original assets to H.264 was tried and explicitly
 * rejected: a port that requires modifying the original game's data files
 * to work isn't a real port. This file decodes the ORIGINAL MPEG-4 Part 2 /
 * AAC files as shipped, entirely in software, via FFmpeg's `libavformat`/
 * `libavcodec`/`libswresample` -- the established approach in PS Vita
 * homebrew for codecs `SceVideodec` doesn't cover. Needs `vdpm ffmpeg`
 * installed once (see CMakeLists.txt's comment next to the ffmpeg
 * `target_link_libraries` entries).
 *
 * Reused from the previous SceAvPlayer-based version, unchanged: the NEON
 * YUV->RGB565 color conversion approach (adapted here for FFmpeg's genuine
 * 3-plane YUV420P output instead of the semi-planar NV12 SceAvPlayer used
 * to hand back), `draw_video_frame()`'s plain GLES1.1 fixed-function
 * texture-quad rendering (do NOT reintroduce a custom GLSL shader here --
 * that caused a confirmed-on-hardware regression this session, see the
 * comment on `draw_video_frame()` below), and the double-buffered
 * `sceAudioOut` VOICE-port cutscene audio output thread.
 */

#include "video.h"
#include "utils/logger.h"
#include "utils/glutil.h"

#include <psp2/ctrl.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/stat.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#include <arm_neon.h>
#include <malloc.h>
#include <pthread.h>
#include <string.h>

static unsigned short *gRgbBuf = NULL;
static unsigned gRgbBufCap = 0;

/**
 * @brief Planar YUV420P (3 separate Y/U/V planes, FFmpeg's native decode
 * output for this codec) to RGB565, using ARM NEON intrinsics.
 *
 * Takes a stride (`AVFrame::linesize`) per plane, separate from the visible
 * width -- FFmpeg decode buffers are commonly padded/aligned wider than the
 * actual frame, unlike the previous SceAvPlayer-based version's assumption
 * of tightly-packed rows (which was fine there because SceAvPlayer handed
 * back exactly-sized buffers, but would silently skew this image if reused
 * as-is here).
 */
static int CV_R[256];
static int CV_G[256];
static int CU_G[256];
static int CU_B[256];
static unsigned char clip_table[768];
static bool tables_init = false;

static void init_yuv_tables() {
    if (tables_init) return;
    for (int i = 0; i < 256; i++) {
        int V = i - 128;
        int U = i - 128;
        CV_R[i] = (91881 * V) >> 16;
        CV_G[i] = (46802 * V) >> 16;
        CU_G[i] = (22554 * U) >> 16;
        CU_B[i] = (116130 * U) >> 16;
    }
    for (int i = 0; i < 768; i++) {
        int v = i - 256;
        clip_table[i] = (v < 0) ? 0 : ((v > 255) ? 255 : v);
    }
    tables_init = true;
}

#define CLIP(X) (clip_table[(X) + 256])

static inline void store_rgb565_8(unsigned short *dst, uint8x8_t r, uint8x8_t g, uint8x8_t b) {
    uint16x8_t rw = vmovl_u8(r);
    uint16x8_t gw = vmovl_u8(g);
    uint16x8_t bw = vmovl_u8(b);
    uint16x8_t rr = vshlq_n_u16(vandq_u16(rw, vdupq_n_u16(0xF8)), 8);
    uint16x8_t gg = vshlq_n_u16(vandq_u16(gw, vdupq_n_u16(0xFC)), 3);
    uint16x8_t bb = vshrq_n_u16(bw, 3);
    vst1q_u16((uint16_t *) dst, vorrq_u16(vorrq_u16(rr, gg), bb));
}

static void yuv420p_planar_to_rgb565(const unsigned char *yPlane, int yStride,
                                      const unsigned char *uPlane, int uStride,
                                      const unsigned char *vPlane, int vStride,
                                      unsigned w, unsigned h, unsigned short *dst) {
    init_yuv_tables();
    for (unsigned y = 0; y < h; y += 2) {
        const unsigned char *yrow0 = yPlane + (size_t) y * yStride;
        const unsigned char *yrow1 = yrow0 + yStride;
        const unsigned char *urow = uPlane + (size_t) (y / 2) * uStride;
        const unsigned char *vrow = vPlane + (size_t) (y / 2) * vStride;
        unsigned short *drow0 = dst + (size_t) y * w;
        unsigned short *drow1 = drow0 + w;

        unsigned x = 0;
        // Each NEON pass covers 16 luma pixels using 8 chroma samples (U and
        // V are already separate planes at half width, no deinterleave
        // needed -- simpler than the old NV12 path this replaces).
        for (; x + 16 <= w; x += 16) {
            uint8x8_t u8 = vld1_u8(urow + x / 2);
            uint8x8_t v8 = vld1_u8(vrow + x / 2);
            int16x8_t Uc = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(u8)), vdupq_n_s16(128));
            int16x8_t Vc = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(v8)), vdupq_n_s16(128));

            int32x4_t Uc_lo = vmovl_s16(vget_low_s16(Uc));
            int32x4_t Uc_hi = vmovl_s16(vget_high_s16(Uc));
            int32x4_t Vc_lo = vmovl_s16(vget_low_s16(Vc));
            int32x4_t Vc_hi = vmovl_s16(vget_high_s16(Vc));

            int16x4_t r_add_lo = vmovn_s32(vshrq_n_s32(vmulq_n_s32(Vc_lo, 91881), 16));
            int16x4_t r_add_hi = vmovn_s32(vshrq_n_s32(vmulq_n_s32(Vc_hi, 91881), 16));

            int32x4_t cu_g_lo = vshrq_n_s32(vmulq_n_s32(Uc_lo, 22554), 16);
            int32x4_t cu_g_hi = vshrq_n_s32(vmulq_n_s32(Uc_hi, 22554), 16);
            int32x4_t cv_g_lo = vshrq_n_s32(vmulq_n_s32(Vc_lo, 46802), 16);
            int32x4_t cv_g_hi = vshrq_n_s32(vmulq_n_s32(Vc_hi, 46802), 16);
            int16x4_t g_add_lo = vneg_s16(vmovn_s32(vaddq_s32(cu_g_lo, cv_g_lo)));
            int16x4_t g_add_hi = vneg_s16(vmovn_s32(vaddq_s32(cu_g_hi, cv_g_hi)));

            int16x4_t b_add_lo = vmovn_s32(vshrq_n_s32(vmulq_n_s32(Uc_lo, 116130), 16));
            int16x4_t b_add_hi = vmovn_s32(vshrq_n_s32(vmulq_n_s32(Uc_hi, 116130), 16));

            int16x8_t r_add8 = vcombine_s16(r_add_lo, r_add_hi);
            int16x8_t g_add8 = vcombine_s16(g_add_lo, g_add_hi);
            int16x8_t b_add8 = vcombine_s16(b_add_lo, b_add_hi);
            int16x8x2_t r_dup = vzipq_s16(r_add8, r_add8);
            int16x8x2_t g_dup = vzipq_s16(g_add8, g_add8);
            int16x8x2_t b_dup = vzipq_s16(b_add8, b_add8);

            for (int half = 0; half < 2; half++) {
                const unsigned char *yr0 = yrow0 + x + half * 8;
                const unsigned char *yr1 = yrow1 + x + half * 8;
                int16x8_t r_add = half == 0 ? r_dup.val[0] : r_dup.val[1];
                int16x8_t g_add = half == 0 ? g_dup.val[0] : g_dup.val[1];
                int16x8_t b_add = half == 0 ? b_dup.val[0] : b_dup.val[1];

                int16x8_t Y0 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(yr0)));
                int16x8_t Y1 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(yr1)));

                uint8x8_t r0 = vqmovun_s16(vaddq_s16(Y0, r_add));
                uint8x8_t g0 = vqmovun_s16(vaddq_s16(Y0, g_add));
                uint8x8_t b0 = vqmovun_s16(vaddq_s16(Y0, b_add));
                uint8x8_t r1 = vqmovun_s16(vaddq_s16(Y1, r_add));
                uint8x8_t g1 = vqmovun_s16(vaddq_s16(Y1, g_add));
                uint8x8_t b1 = vqmovun_s16(vaddq_s16(Y1, b_add));

                store_rgb565_8(drow0 + x + half * 8, r0, g0, b0);
                store_rgb565_8(drow1 + x + half * 8, r1, g1, b1);
            }
        }

        for (; x < w; x += 2) {
            unsigned char U = urow[x / 2];
            unsigned char V = vrow[x / 2];

            int r_add = CV_R[V];
            int g_add = -(CU_G[U] + CV_G[V]);
            int b_add = CU_B[U];

            int Y00 = yrow0[x];
            unsigned char r00 = CLIP(Y00 + r_add), g00 = CLIP(Y00 + g_add), b00 = CLIP(Y00 + b_add);
            drow0[x] = (unsigned short) (((r00 & 0xF8) << 8) | ((g00 & 0xFC) << 3) | (b00 >> 3));

            int Y01 = yrow0[x + 1];
            unsigned char r01 = CLIP(Y01 + r_add), g01 = CLIP(Y01 + g_add), b01 = CLIP(Y01 + b_add);
            drow0[x + 1] = (unsigned short) (((r01 & 0xF8) << 8) | ((g01 & 0xFC) << 3) | (b01 >> 3));

            int Y10 = yrow1[x];
            unsigned char r10 = CLIP(Y10 + r_add), g10 = CLIP(Y10 + g_add), b10 = CLIP(Y10 + b_add);
            drow1[x] = (unsigned short) (((r10 & 0xF8) << 8) | ((g10 & 0xFC) << 3) | (b10 >> 3));

            int Y11 = yrow1[x + 1];
            unsigned char r11 = CLIP(Y11 + r_add), g11 = CLIP(Y11 + g_add), b11 = CLIP(Y11 + b_add);
            drow1[x + 1] = (unsigned short) (((r11 & 0xF8) << 8) | ((g11 & 0xFC) << 3) | (b11 >> 3));
        }
    }
}

static GLuint gVideoTex = 0;
static unsigned gVideoTexW = 0;
static unsigned gVideoTexH = 0;

/*
 * NOT the real 960x544 Vita screen, and NOT the 800x480 the engine itself is
 * told the screen is -- the downsample FBO glutil.c's gl_init() sets up
 * (OFFSCREEN_W/H, defined in glutil.h so this stays in sync automatically)
 * stays bound for the entire program, including while this cutscene player
 * is drawing (nothing here ever rebinds a framebuffer). A viewport/quad
 * sized for anything other than the FBO's actual attachment size would draw
 * past it and get clipped by the GPU, cropping the video into a corner --
 * the same bug class already fixed for the game's own rendering (see Bug
 * #13 in port_progress.md). gl_swap() upscale-blits this FBO's full content
 * (video included) onto the real screen once per frame, so targeting
 * OFFSCREEN_W/H here is correct regardless of what that size is.
 */
#define VIDEO_TARGET_W OFFSCREEN_W
#define VIDEO_TARGET_H OFFSCREEN_H

static bool gFirstDrawLogged = false;
#define FIRST_DRAW_LOG(...) do { if (!gFirstDrawLogged) l_info(__VA_ARGS__); } while (0)

/*
 * Draws one decoded video frame (already CPU-converted to RGB565 by
 * yuv420p_planar_to_rgb565()) as a letterboxed quad, using PLAIN GLES1.1
 * fixed-function texturing -- deliberately NOT a custom GLSL program with
 * generic vertex attribute arrays, which an earlier version of this
 * function did (glUseProgram(customProgram) + glVertexAttribPointer on
 * attribute locations 0/1, doing the YUV->RGB conversion in a fragment
 * shader instead of on the CPU here).
 *
 * That GLES2 version caused a real, confirmed-on-hardware regression: the
 * first time it ever actually ran, every fixed-function draw for the rest
 * of the program's life -- the loading screen, the title screen -- came out
 * solid white, while the menu (reached later, evidently through a
 * different code path or one that resets more GL state itself) still
 * worked. `libasphalt5.so` is GLES1.1 fixed-function only and runs through
 * vitaGL, a translation layer that implements the fixed-function pipeline
 * via its OWN internally-managed shader machinery, layered UNDER the same
 * `glUseProgram`/vertex-attribute API surface real custom shaders use.
 * Keep this on the same plain fixed-function path as
 * `gl_blit_downsample_to_screen()` in glutil.c -- the other quad blit in
 * this codebase, hardware-proven every single frame -- rather than
 * reintroducing that risk.
 */
static void draw_video_frame(const unsigned short *rgb565, unsigned w, unsigned h) {
    FIRST_DRAW_LOG("video: draw_video_frame ENTER (%ux%u)", w, h);

    if (!gVideoTex || gVideoTexW != w || gVideoTexH != h) {
        FIRST_DRAW_LOG("video: creating texture storage...");
        if (!gVideoTex) glGenTextures(1, &gVideoTex);
        glBindTexture(GL_TEXTURE_2D, gVideoTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        FIRST_DRAW_LOG("video: about to glTexImage2D (%ux%u, RGB565)...", w, h);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (GLsizei) w, (GLsizei) h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
        FIRST_DRAW_LOG("video: glTexImage2D returned (err=0x%04x)", glGetError());
        gVideoTexW = w;
        gVideoTexH = h;
    }
    glBindTexture(GL_TEXTURE_2D, gVideoTex);
    FIRST_DRAW_LOG("video: about to glTexSubImage2D...");
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei) w, (GLsizei) h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, rgb565);
    FIRST_DRAW_LOG("video: glTexSubImage2D returned (err=0x%04x)", glGetError());

    float srcAspect = (float) w / (float) h;
    float dstAspect = (float) VIDEO_TARGET_W / (float) VIDEO_TARGET_H;
    float qx0 = 0, qy0 = 0, qx1 = VIDEO_TARGET_W, qy1 = VIDEO_TARGET_H;
    if (srcAspect > dstAspect) {
        float qh = VIDEO_TARGET_W / srcAspect;
        qy0 = (VIDEO_TARGET_H - qh) / 2.0f;
        qy1 = qy0 + qh;
    } else {
        float qw = VIDEO_TARGET_H * srcAspect;
        qx0 = (VIDEO_TARGET_W - qw) / 2.0f;
        qx1 = qx0 + qw;
    }

    float nx0 = qx0 / VIDEO_TARGET_W * 2.0f - 1.0f;
    float nx1 = qx1 / VIDEO_TARGET_W * 2.0f - 1.0f;
    float ny0 = 1.0f - qy0 / VIDEO_TARGET_H * 2.0f;
    float ny1 = 1.0f - qy1 / VIDEO_TARGET_H * 2.0f;

    // Static storage, not a stack array -- client-side vertex pointers are
    // only guaranteed read AT glDrawArrays in a spec-strict implementation;
    // gl_blit_downsample_to_screen() in glutil.c uses `static const` for the
    // same reason. Contents vary per call (aspect ratio depends on the
    // video), so this can't also be `const` the way that fixed fullscreen
    // quad is.
    static GLfloat verts[8];
    verts[0] = nx0; verts[1] = ny0;
    verts[2] = nx1; verts[3] = ny0;
    verts[4] = nx0; verts[5] = ny1;
    verts[6] = nx1; verts[7] = ny1;
    static const GLfloat uvs[8] = {
        0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f,  1.0f, 1.0f,
    };

    FIRST_DRAW_LOG("video: about to save GL state...");
    GLboolean savedBlend = glIsEnabled(GL_BLEND);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean savedScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean savedCull = glIsEnabled(GL_CULL_FACE);
    GLboolean savedTex2D = glIsEnabled(GL_TEXTURE_2D);
    GLboolean savedVertexArray = glIsEnabled(GL_VERTEX_ARRAY);
    GLboolean savedTexCoordArray = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
    GLint savedViewport[4];
    glGetIntegerv(GL_VIEWPORT, savedViewport);
    GLint savedTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex);
    FIRST_DRAW_LOG("video: GL state saved (blend=%d depth=%d scissor=%d cull=%d tex=%d)",
                   savedBlend, savedDepthTest, savedScissor, savedCull, savedTex);

    glViewport(0, 0, VIDEO_TARGET_W, VIDEO_TARGET_H);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    FIRST_DRAW_LOG("video: viewport/disables done");

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, gVideoTex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);
    FIRST_DRAW_LOG("video: about to glDrawArrays...");
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    FIRST_DRAW_LOG("video: glDrawArrays returned (err=0x%04x)", glGetError());
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    if (!gFirstDrawLogged) {
        unsigned char pixel[4] = {0, 0, 0, 0};
        glReadPixels(VIDEO_TARGET_W / 2, VIDEO_TARGET_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        l_info("[video_diag] framebuffer readback at center, right after glDrawArrays: rgba=%u,%u,%u,%u (err=0x%04x)",
               pixel[0], pixel[1], pixel[2], pixel[3], glGetError());
    }

    FIRST_DRAW_LOG("video: about to gl_swap()...");
    gl_swap();
    FIRST_DRAW_LOG("video: gl_swap() returned -- first frame fully presented");

    glBindTexture(GL_TEXTURE_2D, (GLuint) savedTex);
    if (savedBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (savedDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (savedScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (savedCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);

    // Restore client arrays
    if (savedVertexArray) glEnableClientState(GL_VERTEX_ARRAY); else glDisableClientState(GL_VERTEX_ARRAY);
    if (savedTexCoordArray) glEnableClientState(GL_TEXTURE_COORD_ARRAY); else glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    // Engine expects texture env mode to be MODULATE by default (we forced REPLACE)
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    // We disabled texture 2D at the end of drawing, restore it to its original state
    if (savedTex2D) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);

    FIRST_DRAW_LOG("video: GL state restored");
    gFirstDrawLogged = true;
}

/**
 * @brief Dedicated cutscene audio output thread using sceAudioOut (VOICE port).
 *
 * Unchanged from the SceAvPlayer-based version. This double-buffered
 * blocking design is also what naturally paces DECODE of audio to real
 * time: `cutscene_audio_submit()` blocks (retrying every 500us) once both
 * buffer slots are full, and `sceAudioOutOutput()` itself blocks until the
 * hardware is ready to accept the next chunk -- so a decode loop calling
 * `cutscene_audio_submit()` as fast as it can decode packets will still
 * only ever run ~1 buffer ahead of real playback. Video has no equivalent
 * built-in backpressure (see the PTS-pacing comment in video_play() below).
 */
static pthread_mutex_t gCutAudioLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *gCutAudioBuf[2] = { NULL, NULL };
static unsigned gCutAudioBufCap = 0;
static unsigned gCutAudioLen[2] = { 0, 0 };
static int gCutAudioWriteSlot = 0;
static int gCutAudioPort = -1;
static volatile bool gCutAudioQuit = false;

static int cutscene_audio_thread(SceSize args, void *argp) {
    (void) args; (void) argp;
    int slot = 0;
    for (;;) {
        pthread_mutex_lock(&gCutAudioLock);
        unsigned len = gCutAudioLen[slot];
        bool quit = gCutAudioQuit;
        pthread_mutex_unlock(&gCutAudioLock);

        if (len == 0) {
            if (quit)
                break;
            sceKernelDelayThread(500);
            continue;
        }

        if (gCutAudioPort >= 0)
            sceAudioOutOutput(gCutAudioPort, gCutAudioBuf[slot]);
        pthread_mutex_lock(&gCutAudioLock);
        gCutAudioLen[slot] = 0;
        pthread_mutex_unlock(&gCutAudioLock);
        slot ^= 1;
    }
    return 0;
}

static void cutscene_audio_submit(const void *pData, unsigned bytes) {
    if (bytes > gCutAudioBufCap) {
        free(gCutAudioBuf[0]);
        free(gCutAudioBuf[1]);
        gCutAudioBuf[0] = (unsigned char *) malloc(bytes);
        gCutAudioBuf[1] = (unsigned char *) malloc(bytes);
        gCutAudioBufCap = (gCutAudioBuf[0] && gCutAudioBuf[1]) ? bytes : 0;
    }
    if (!gCutAudioBuf[0] || !gCutAudioBuf[1] || gCutAudioBufCap < bytes)
        return;

    for (;;) {
        pthread_mutex_lock(&gCutAudioLock);
        bool free_slot = gCutAudioLen[gCutAudioWriteSlot] == 0;
        if (free_slot) {
            memcpy(gCutAudioBuf[gCutAudioWriteSlot], pData, bytes);
            gCutAudioLen[gCutAudioWriteSlot] = bytes;
        }
        pthread_mutex_unlock(&gCutAudioLock);
        if (free_slot)
            break;
        sceKernelDelayThread(500);
    }
    gCutAudioWriteSlot ^= 1;
}

/*
 * FFmpeg >= 4.0 (libavformat >= 58) registers every muxer/demuxer/codec
 * automatically; av_register_all()/avcodec_register_all() were removed
 * (calling them is a compile error, not just a no-op) in that version and
 * later. This guard covers whichever the vita-portlibs `ffmpeg` package
 * (installed via `vdpm ffmpeg`) actually ships -- NOT verified in this
 * environment (no VITASDK installed here at all), check on first build:
 * if this file fails to compile with an "undefined reference"/"implicit
 * declaration" around av_register_all, the installed version is >= 4.0 and
 * this whole block is correctly compiled out already; if instead nothing
 * ever plays and no error/warning from FFmpeg ever appears in the log, a
 * PRE-4.0 version may need this block force-enabled.
 */
#if LIBAVFORMAT_VERSION_MAJOR < 58
static void ffmpeg_register_once() {
    static bool done = false;
    if (done) return;
    av_register_all();
    done = true;
}
#else
static void ffmpeg_register_once() { /* automatic since ffmpeg 4.0 */ }
#endif

void video_init() {
    ffmpeg_register_once();
    l_success("video: FFmpeg software decoder ready.");
}

void video_shutdown() {
    if (gVideoTex) {
        glDeleteTextures(1, &gVideoTex);
        gVideoTex = 0;
        gVideoTexW = 0;
        gVideoTexH = 0;
    }
    free(gRgbBuf);
    gRgbBuf = NULL;
    gRgbBufCap = 0;
}

/*
 * Resolves `name` (a bare filename, e.g. "A5_Ultimate_VNFS_2.mp4" --
 * confirmed via GS_TrailerMovie::Create() in the decompiled engine) against
 * the same set of candidate locations the port has always used for this.
 * Unchanged from the SceAvPlayer-based version.
 */
static bool resolve_video_path(const char *name, char *path, size_t pathSize) {
    SceIoStat st;
    const char *prefixes[] = {
        DATA_PATH "%s",
        DATA_PATH "data/%s",
        DATA_PATH "data/%s.mp4",
        DATA_PATH "%s.mp4",
        DATA_PATH "files/%s",
        DATA_PATH "res/raw/%s",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        snprintf(path, pathSize, prefixes[i], name);
        if (sceIoGetstat(path, &st) >= 0)
            return true;
    }
    return false;
}

/**
 * @brief Plays a video file via software MPEG-4/AAC decode.
 * @param name File name or relative path of the video cutscene to play.
 */
void video_play(const char *name) {
    if (!name) {
        l_warn("video: video_play() called with a null name, skipping");
        return;
    }

    char path[512];
    if (!resolve_video_path(name, path, sizeof(path))) {
        l_error("video: file not found for \"%s\" (searched DATA_PATH data/, files/, res/raw/)", name);
        return;
    }

    AVFormatContext *fmtCtx = NULL;
    // Plain filesystem path (e.g. "ux0:data/asphalt5/data/foo.mp4"), no
    // custom AVIOContext -- FFmpeg's default "file" protocol goes through
    // this toolchain's own libc fopen/lseek/read (source/reimpl/io.c),
    // already proven to handle ux0:-prefixed absolute paths correctly
    // everywhere else in this port. This also means backward seeks (needed
    // for these files' moov atom sitting at EOF, not faststart-optimized)
    // just work, the same as any other local file read in this project.
    if (avformat_open_input(&fmtCtx, path, NULL, NULL) != 0) {
        l_error("video: avformat_open_input failed for %s", path);
        return;
    }
    if (avformat_find_stream_info(fmtCtx, NULL) < 0) {
        l_error("video: avformat_find_stream_info failed for %s", path);
        avformat_close_input(&fmtCtx);
        return;
    }

    int videoStreamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int audioStreamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    AVCodecContext *vCtx = NULL;
    AVCodecContext *aCtx = NULL;

    if (videoStreamIdx >= 0) {
        AVCodecParameters *par = fmtCtx->streams[videoStreamIdx]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(par->codec_id);
        if (!codec) {
            l_error("video: no software decoder registered for video codec id %d -- is the "
                    "vdpm ffmpeg build's decoder list missing mpeg4? (see CMakeLists.txt comment)",
                    (int) par->codec_id);
        } else {
            vCtx = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(vCtx, par);
            if (avcodec_open2(vCtx, codec, NULL) < 0) {
                l_error("video: avcodec_open2 failed for video stream");
                avcodec_free_context(&vCtx);
                vCtx = NULL;
            }
        }
    }
    if (audioStreamIdx >= 0) {
        AVCodecParameters *par = fmtCtx->streams[audioStreamIdx]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(par->codec_id);
        if (!codec) {
            l_warn("video: no software decoder registered for audio codec id %d -- cutscene will be silent",
                   (int) par->codec_id);
        } else {
            aCtx = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(aCtx, par);
            if (avcodec_open2(aCtx, codec, NULL) < 0) {
                l_warn("video: avcodec_open2 failed for audio stream -- cutscene will be silent");
                avcodec_free_context(&aCtx);
                aCtx = NULL;
            }
        }
    }

    if (!vCtx) {
        l_error("video: no usable video stream/decoder for %s, aborting playback", path);
        if (aCtx) avcodec_free_context(&aCtx);
        avformat_close_input(&fmtCtx);
        return;
    }

    l_success("video: playing %s (%dx%d %s, %s audio)", path, vCtx->width, vCtx->height,
              avcodec_get_name(vCtx->codec_id), aCtx ? avcodec_get_name(aCtx->codec_id) : "no");

    // -------- audio resampler: decoder's native format -> interleaved S16,
    // whatever channel count/rate it decoded to (confirmed 44100/stereo for
    // this game's assets, but not hardcoded in case a future asset differs)
    // for the sceAudioOut VOICE port. --------
    SwrContext *swr = NULL;
    int audioOutChannels = 0;
    int audioOutRate = 0;
    if (aCtx) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
        // FFmpeg >= 5.1's AVChannelLayout API (channels/channel_layout
        // fields were removed from AVCodecContext in later cleanups).
        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, aCtx->ch_layout.nb_channels > 0 ? aCtx->ch_layout.nb_channels : 2);
        int rc = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, aCtx->sample_rate,
                                      &aCtx->ch_layout, aCtx->sample_fmt, aCtx->sample_rate, 0, NULL);
        audioOutChannels = outLayout.nb_channels;
        av_channel_layout_uninit(&outLayout);
        if (rc < 0 || !swr || swr_init(swr) < 0) {
            l_warn("video: swr_alloc_set_opts2/swr_init failed -- cutscene will be silent");
            if (swr) { swr_free(&swr); swr = NULL; }
        }
#else
        int64_t inLayout = aCtx->channel_layout ? (int64_t) aCtx->channel_layout
                                                 : av_get_default_channel_layout(aCtx->channels);
        audioOutChannels = aCtx->channels > 0 ? aCtx->channels : 2;
        swr = swr_alloc_set_opts(NULL, inLayout, AV_SAMPLE_FMT_S16, aCtx->sample_rate,
                                  inLayout, aCtx->sample_fmt, aCtx->sample_rate, 0, NULL);
        if (!swr || swr_init(swr) < 0) {
            l_warn("video: swr_alloc_set_opts/swr_init failed -- cutscene will be silent");
            if (swr) { swr_free(&swr); swr = NULL; }
        }
#endif
        audioOutRate = aCtx->sample_rate;
    }

    int audioPort = -1;
    SceUID cutAudioThreadUid = -1;
    bool audioPortOpenAttempted = false;
    unsigned char *audioOutBuf = NULL;
    unsigned audioOutBufCap = 0;

    bool skipped = false;
    int video_frames = 0, audio_frames = 0;
    uint64_t convert_us_total = 0, draw_us_total = 0;

    SceCtrlData pad_start;
    sceCtrlPeekBufferPositive(0, &pad_start, 1);
    uint32_t old_pad_buttons = pad_start.buttons;

    uint64_t play_start_time = sceKernelGetProcessTimeWide();
    AVRational vTimeBase = fmtCtx->streams[videoStreamIdx]->time_base;

    l_info("video: decode loop starting for %s", path);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *swFrame = NULL; // only allocated if a hw pixel format ever shows up (shouldn't, software decode)

    bool eof = false;
    while (!skipped) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t pressed = pad.buttons & ~old_pad_buttons;
        if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_START)) {
            l_info("video: skipped by user button press! (pad=0x%08X)", (unsigned) pad.buttons);
            skipped = true;
            break;
        }
        old_pad_buttons = pad.buttons;

        if (!eof) {
            int rr = av_read_frame(fmtCtx, pkt);
            if (rr < 0) {
                eof = true;
                // Flush both decoders (send a NULL packet) so any frames
                // buffered inside them (common for B-frame-using codecs)
                // still get decoded/drawn instead of silently dropped.
                avcodec_send_packet(vCtx, NULL);
                if (aCtx) avcodec_send_packet(aCtx, NULL);
            } else if (pkt->stream_index == videoStreamIdx) {
                avcodec_send_packet(vCtx, pkt);
                av_packet_unref(pkt);
            } else if (aCtx && pkt->stream_index == audioStreamIdx) {
                avcodec_send_packet(aCtx, pkt);
                av_packet_unref(pkt);
            } else {
                av_packet_unref(pkt);
            }
        }

        // Drain every video frame currently available from the decoder.
        for (;;) {
            int rc = avcodec_receive_frame(vCtx, frame);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
                break;
            if (rc < 0) {
                l_warn("video: avcodec_receive_frame(video) error 0x%08x", rc);
                break;
            }

            AVFrame *f = frame;
            if (f->format != AV_PIX_FMT_YUV420P) {
                // Software mpeg4 decode should always hand back planar
                // YUV420P directly -- this project has no swscale
                // conversion path wired up, so anything else (a hw pixel
                // format slipping through, or an unusual profile) is
                // logged and dropped rather than silently misread as
                // YUV420P and drawn as corrupted colors/garbage.
                l_warn("video: decoded frame has unexpected pix_fmt %d (expected YUV420P/%d) -- dropping frame",
                       (int) f->format, (int) AV_PIX_FMT_YUV420P);
                av_frame_unref(f);
                continue;
            }

            if (++video_frames == 1)
                l_info("video: first video frame decoded (%dx%d)", f->width, f->height);

            // Pace video to the stream's own presentation timestamps --
            // nothing else in this loop throttles decode/draw to real time
            // for video specifically (audio self-throttles via the
            // blocking double-buffered sceAudioOut path above). Without
            // this, decode+draw would run as fast as the CPU allows, which
            // is almost certainly much faster than the video's real frame
            // rate, and gl_swap()'s vsync wait only caps the upper bound
            // (60Hz), not paces to whatever lower rate the source actually
            // is.
            if (f->pts != AV_NOPTS_VALUE) {
                double pts_sec = f->pts * av_q2d(vTimeBase);
                uint64_t target_us = play_start_time + (uint64_t) (pts_sec * 1000000.0);
                uint64_t now = sceKernelGetProcessTimeWide();
                if (target_us > now) {
                    uint64_t wait_us = target_us - now;
                    // Cap a single wait so a bad/out-of-order timestamp
                    // can't stall skip-button responsiveness for long --
                    // re-checked against real time on the next loop
                    // iteration regardless.
                    if (wait_us > 200000) wait_us = 200000;
                    sceKernelDelayThread((SceUInt) wait_us);
                }
            }

            unsigned w = (unsigned) f->width, h = (unsigned) f->height;
            unsigned need = w * h * sizeof(unsigned short);
            if (need > gRgbBufCap) {
                free(gRgbBuf);
                gRgbBuf = (unsigned short *) malloc(need);
                gRgbBufCap = gRgbBuf ? need : 0;
            }
            if (gRgbBuf && gRgbBufCap >= need) {
                uint64_t t0 = sceKernelGetProcessTimeWide();
                yuv420p_planar_to_rgb565(f->data[0], f->linesize[0],
                                          f->data[1], f->linesize[1],
                                          f->data[2], f->linesize[2],
                                          w, h, gRgbBuf);
                uint64_t t1 = sceKernelGetProcessTimeWide();
                draw_video_frame(gRgbBuf, w, h);
                uint64_t t2 = sceKernelGetProcessTimeWide();
                convert_us_total += t1 - t0;
                draw_us_total += t2 - t1;
            }
            av_frame_unref(f);
        }

        // Drain every audio frame currently available from the decoder.
        if (aCtx) {
            for (;;) {
                int rc = avcodec_receive_frame(aCtx, frame);
                if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
                    break;
                if (rc < 0) {
                    l_warn("video: avcodec_receive_frame(audio) error 0x%08x", rc);
                    break;
                }

                AVFrame *f = frame;
                if (++audio_frames == 1)
                    l_info("video: first audio frame decoded (ch=%d rate=%d)",
                           audioOutChannels, aCtx->sample_rate);

                if (swr) {
                    int outSamples = swr_get_out_samples(swr, f->nb_samples);
                    if (outSamples < 0) outSamples = f->nb_samples * 2;
                    unsigned need = (unsigned) outSamples * audioOutChannels * sizeof(int16_t);
                    if (need > audioOutBufCap) {
                        free(audioOutBuf);
                        audioOutBuf = (unsigned char *) malloc(need);
                        audioOutBufCap = audioOutBuf ? need : 0;
                    }
                    if (audioOutBuf && audioOutBufCap >= need) {
                        uint8_t *outPlanes[1] = { audioOutBuf };
                        int converted = swr_convert(swr, outPlanes, outSamples,
                                                     (const uint8_t **) f->extended_data, f->nb_samples);
                        if (converted > 0) {
                            unsigned bytes = (unsigned) converted * audioOutChannels * sizeof(int16_t);

                            if (audioPort < 0 && !audioPortOpenAttempted) {
                                audioPortOpenAttempted = true;
                                SceAudioOutMode mode = (audioOutChannels >= 2) ? SCE_AUDIO_OUT_MODE_STEREO
                                                                                : SCE_AUDIO_OUT_MODE_MONO;
                                // sceAudioOutOpenPort's `len` is frames per
                                // channel and (per Vita audio driver
                                // convention) must be a multiple of 64;
                                // round the very first converted chunk's
                                // size up rather than assume a fixed value.
                                unsigned lenFrames = ((unsigned) converted + 63u) & ~63u;
                                if (lenFrames == 0) lenFrames = 64;
                                l_info("video: cutscene audio port: %u frames/channel, rate=%d, ch=%d",
                                       lenFrames, audioOutRate, audioOutChannels);
                                audioPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_VOICE, (int) lenFrames,
                                                                 audioOutRate, mode);
                                if (audioPort < 0) {
                                    l_warn("video: sceAudioOutOpenPort for cutscene audio failed (0x%08X) -- "
                                           "cutscene audio disabled", (unsigned) audioPort);
                                } else {
                                    gCutAudioPort = audioPort;
                                    gCutAudioWriteSlot = 0;
                                    gCutAudioLen[0] = 0;
                                    gCutAudioLen[1] = 0;
                                    gCutAudioQuit = false;
                                    cutAudioThreadUid = sceKernelCreateThread("cutscene audio out", cutscene_audio_thread,
                                                                               0x40, 0x4000, 0, 0x40000, NULL);
                                    if (cutAudioThreadUid >= 0) {
                                        sceKernelStartThread(cutAudioThreadUid, 0, NULL);
                                    } else {
                                        l_warn("video: cutscene audio thread creation failed (0x%08X) -- "
                                               "cutscene audio disabled", (unsigned) cutAudioThreadUid);
                                        sceAudioOutReleasePort(audioPort);
                                        audioPort = -1;
                                        gCutAudioPort = -1;
                                    }
                                }
                            }

                            if (audioPort >= 0)
                                cutscene_audio_submit(audioOutBuf, bytes);
                        }
                    }
                }
                av_frame_unref(f);
            }
        }

        if (eof) {
            // Both decoders were flushed above; once a flush pass yields
            // no more frames from either, playback genuinely ended.
            l_info("video: end of stream reached naturally.");
            break;
        }
    }

    uint64_t play_end_time = sceKernelGetProcessTimeWide();
    double elapsed_sec = (double) (play_end_time - play_start_time) / 1000000.0;
    double avg_fps = elapsed_sec > 0.0 ? (double) video_frames / elapsed_sec : 0.0;
    double convert_ms_per_frame = video_frames > 0 ? ((double) convert_us_total / 1000.0) / video_frames : 0.0;
    double draw_ms_per_frame = video_frames > 0 ? ((double) draw_us_total / 1000.0) / video_frames : 0.0;
    l_info("video: loop exited! video_frames=%d, audio_frames=%d, elapsed=%.2fs, avg_fps=%.1f, "
           "yuv_convert=%.1fms/frame, tex_upload+gl_draw+swap=%.1fms/frame",
           video_frames, audio_frames, elapsed_sec, avg_fps, convert_ms_per_frame, draw_ms_per_frame);

    if (cutAudioThreadUid >= 0) {
        pthread_mutex_lock(&gCutAudioLock);
        gCutAudioQuit = true;
        pthread_mutex_unlock(&gCutAudioLock);
        sceKernelWaitThreadEnd(cutAudioThreadUid, NULL, NULL);
        sceKernelDeleteThread(cutAudioThreadUid);
    }
    gCutAudioPort = -1;
    if (audioPort >= 0)
        sceAudioOutReleasePort(audioPort);
    free(audioOutBuf);

    if (swFrame) av_frame_free(&swFrame);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    if (swr) swr_free(&swr);
    avcodec_free_context(&vCtx);
    if (aCtx) avcodec_free_context(&aCtx);
    avformat_close_input(&fmtCtx);

    l_success("video: %s (%s)", skipped ? "skipped" : "finished", path);
}
