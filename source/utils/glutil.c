/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"

#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <stdbool.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);

void gl_preload() {
    if (!file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }

#ifdef USE_GLSL_SHADERS
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
#endif
}

// Real physical Vita screen -- what vglInitExtended() below actually targets.
#define REAL_SCREEN_W 960
#define REAL_SCREEN_H 544

/*
 * Resolution reported to the engine (see main.c's SCREEN_W/H, told to it via
 * Renderer_nativeInit()/OS_SCREEN_H) -- its menu/UI layout needs exactly
 * this value, confirmed on hardware: reporting the real 960x544 broke menu
 * layout. Every glViewport/glScissor call the engine issues is expressed in
 * THIS space -- glViewport_soloader()/glScissor_soloader() below rescale
 * them onto the actual (smaller) OFFSCREEN_W/H FBO declared in glutil.h.
 */
#define SCREEN_W 800
#define SCREEN_H 480

static GLuint s_ds_fbo = 0;
static GLuint s_ds_color_tex = 0;
static GLuint s_ds_depth_rb = 0;

static void gl_init_downsample() {
    glGenTextures(1, &s_ds_color_tex);
    glBindTexture(GL_TEXTURE_2D, s_ds_color_tex);
    // GL_RGBA, not GL_RGB -- GL_RGBA8 is the one format every GLES
    // implementation is required to support as an FBO color attachment.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, OFFSCREEN_W, OFFSCREEN_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &s_ds_depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, s_ds_depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, OFFSCREEN_W, OFFSCREEN_H);

    glGenFramebuffers(1, &s_ds_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_ds_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_ds_color_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_ds_depth_rb);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        l_error("Downsample FBO incomplete (0x%04x) -- rendering straight to "
                "the real framebuffer instead.", status);
        glDeleteFramebuffers(1, &s_ds_fbo);
        s_ds_fbo = 0;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    l_success("Downsample FBO ready: %dx%d (reported %dx%d, native %dx%d).",
              OFFSCREEN_W, OFFSCREEN_H, SCREEN_W, SCREEN_H, REAL_SCREEN_W, REAL_SCREEN_H);

    // The color texture was allocated via glTexImage2D(..., NULL) -- its
    // initial content is undefined GPU memory, not guaranteed black. Force
    // a real clear now so the very first gl_swap() (before anything has
    // necessarily drawn a full frame into this FBO yet) blits something
    // deterministic instead of whatever was already sitting in that memory.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Left bound on purpose: the engine never binds a framebuffer of its own
    // (this whole port's fixed-function GLES1.1 code always draws to
    // "the" framebuffer), so every one of its draw calls -- for the rest of
    // the program's life -- lands in here without it knowing the difference.
}

void gl_init() {
    // FalsoJNI might pass a lot of data, and 6MB might not be enough for Asphalt 5. Let's use 12MB.
    vglInitExtended(0, 960, 544, 12 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    gl_init_downsample();
}

/*
 * Rescale a rect from the 800x480 space the engine was told the screen is
 * (SCREEN_W/H) onto the actual OFFSCREEN_W/H FBO attachment. Multiply before
 * divide (never divide first) to keep rounding error small, and clamp the
 * result into the FBO's bounds so a rect that lands exactly on the 800/480
 * edge can never produce an out-of-range viewport/scissor rect for GXM --
 * that combination (undersized real framebuffer + an unclamped rescaled
 * rect) was the leading suspect for the GPU crash in the reverted Bug #8
 * attempt. 720/800 and 432/480 both reduce to exactly 9/10, so this is exact
 * (no rounding at all) for the common full-screen case and any rect whose
 * edges are multiples of 10 in engine space.
 */
static void rescale_to_offscreen(GLint x, GLint y, GLsizei w, GLsizei h,
                                  GLint * out_x, GLint * out_y,
                                  GLsizei * out_w, GLsizei * out_h) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    GLint rx = (x * OFFSCREEN_W) / SCREEN_W;
    GLint ry = (y * OFFSCREEN_H) / SCREEN_H;
    GLsizei rw = (w * OFFSCREEN_W) / SCREEN_W;
    GLsizei rh = (h * OFFSCREEN_H) / SCREEN_H;

    if (rx > OFFSCREEN_W) rx = OFFSCREEN_W;
    if (ry > OFFSCREEN_H) ry = OFFSCREEN_H;
    if (rw > OFFSCREEN_W - rx) rw = OFFSCREEN_W - rx;
    if (rh > OFFSCREEN_H - ry) rh = OFFSCREEN_H - ry;

    *out_x = rx;
    *out_y = ry;
    *out_w = rw;
    *out_h = rh;
}

void glViewport_soloader(GLint x, GLint y, GLsizei width, GLsizei height) {
    GLint rx, ry;
    GLsizei rw, rh;
    rescale_to_offscreen(x, y, width, height, &rx, &ry, &rw, &rh);
    glViewport(rx, ry, rw, rh);
}

void glScissor_soloader(GLint x, GLint y, GLsizei width, GLsizei height) {
    GLint rx, ry;
    GLsizei rw, rh;
    rescale_to_offscreen(x, y, width, height, &rx, &ry, &rw, &rh);
    glScissor(rx, ry, rw, rh);
}

// See the comment on the matching externs in glutil.h -- only updated by the
// engine's own calls (routed here via dynlib.c's import table), never by
// this port's own GL calls, which link the real vitaGL entry points instead
// of these wrappers. GLES1.1 defaults: all disabled.
GLboolean g_shadow_depth_test   = GL_FALSE;
GLboolean g_shadow_blend        = GL_FALSE;
GLboolean g_shadow_scissor_test = GL_FALSE;
GLboolean g_shadow_cull_face    = GL_FALSE;
GLboolean g_shadow_vertex_array = GL_FALSE;

void glEnable_soloader(GLenum cap) {
    switch (cap) {
        case GL_DEPTH_TEST:    g_shadow_depth_test   = GL_TRUE; break;
        case GL_BLEND:         g_shadow_blend        = GL_TRUE; break;
        case GL_SCISSOR_TEST:  g_shadow_scissor_test = GL_TRUE; break;
        case GL_CULL_FACE:     g_shadow_cull_face    = GL_TRUE; break;
        default: break;
    }
    glEnable(cap);
}

void glDisable_soloader(GLenum cap) {
    switch (cap) {
        case GL_DEPTH_TEST:    g_shadow_depth_test   = GL_FALSE; break;
        case GL_BLEND:         g_shadow_blend        = GL_FALSE; break;
        case GL_SCISSOR_TEST:  g_shadow_scissor_test = GL_FALSE; break;
        case GL_CULL_FACE:     g_shadow_cull_face    = GL_FALSE; break;
        default: break;
    }
    glDisable(cap);
}

void glEnableClientState_soloader(GLenum array) {
    if (array == GL_VERTEX_ARRAY) g_shadow_vertex_array = GL_TRUE;
    glEnableClientState(array);
}

void glDisableClientState_soloader(GLenum array) {
    if (array == GL_VERTEX_ARRAY) g_shadow_vertex_array = GL_FALSE;
    glDisableClientState(array);
}

// One upscale blit from the downsample FBO onto the real 960x544 screen,
// then present. A plain non-uniform stretch (OFFSCREEN is 5:3, the real
// screen is ~16:9) -- same stretch ratios the engine's own reported
// resolution already implied before this change, so it looks the same as
// what was already on screen, just genuinely cheaper to render.
static void gl_blit_downsample_to_screen() {
    // SAVE STATE
    // Viewport and the 4 boolean caps below come from the shadow state that
    // glViewport_soloader() (well, glViewport itself is left un-shadowed --
    // see below)/glEnable_soloader()/glDisable_soloader() maintain, instead
    // of glGetIntegerv()/glIsEnabled() round-trips every single frame. The
    // engine's own glViewport calls already went through glViewport_soloader
    // to get rescaled onto OFFSCREEN_W/H (see above), so their *result* is
    // exactly what plain glGetIntegerv(GL_VIEWPORT) would have returned
    // anyway -- shadowing that one too would just be duplicating state GL
    // already holds for free, so it stays a real query.
    GLint old_vp[4];
    glGetIntegerv(GL_VIEWPORT, old_vp);
    GLboolean depth_test = g_shadow_depth_test;
    GLboolean blend = g_shadow_blend;
    GLboolean scissor = g_shadow_scissor_test;
    GLboolean cull = g_shadow_cull_face;
    GLboolean tex2d = glIsEnabled(GL_TEXTURE_2D);

    GLint old_tex;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);

    GLint old_env;
    glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &old_env);

    GLfloat old_color[4];
    glGetFloatv(GL_CURRENT_COLOR, old_color);

    // GL_VERTEX_ARRAY isn't per-texture-unit, so its shadow is always
    // correct; GL_TEXTURE_COORD_ARRAY IS per glClientActiveTexture unit and
    // the engine does use multitexturing, so it stays a real query (see the
    // comment on the shadow externs in glutil.h).
    GLboolean v_array = g_shadow_vertex_array;
    GLboolean t_array = glIsEnabled(GL_TEXTURE_COORD_ARRAY);

    // BLIT
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glViewport(0, 0, REAL_SCREEN_W, REAL_SCREEN_H);

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, s_ds_color_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    static const GLfloat verts[8] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };
    static const GLfloat uvs[8] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f,
    };
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    
    // RESTORE STATE
    if (!v_array) glDisableClientState(GL_VERTEX_ARRAY);
    if (!t_array) glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    
    glColor4f(old_color[0], old_color[1], old_color[2], old_color[3]);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, old_env);
    glBindTexture(GL_TEXTURE_2D, old_tex);
    
    if (!tex2d) glDisable(GL_TEXTURE_2D);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (blend) glEnable(GL_BLEND);
    if (depth_test) glEnable(GL_DEPTH_TEST);
    glViewport(old_vp[0], old_vp[1], old_vp[2], old_vp[3]);
}

void gl_swap() {
    if (s_ds_fbo) {
        gl_blit_downsample_to_screen();
        vglSwapBuffers(GL_FALSE);
        glBindFramebuffer(GL_FRAMEBUFFER, s_ds_fbo);
    } else {
        vglSwapBuffers(GL_FALSE);
    }
}

void glCopyTexImage2D_soloader(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border) {
    // Allocate a 1x1 dummy texture to satisfy the GPU without leaking memory or killing FPS
    glTexImage2D(target, level, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

void glCopyTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height) {
    // No-op to avoid slow CPU readbacks
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glShaderSource<%p>(shader: %i, count: %i, string: %p, length: %p)\n", __builtin_return_address(0), shader, count, string, _length);
#endif
    if (!string) {
        l_error("<%p> Shader source string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    } else if (!*string) {
        l_error("<%p> Shader source *string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    }

    size_t total_length = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            total_length += strlen(string[i]);
        } else {
            total_length += _length[i];
        }
    }

    char * str = malloc(total_length+1);
    size_t l = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            memcpy(str + l, string[i], strlen(string[i]));
            l += strlen(string[i]);
        } else {
            memcpy(str + l, string[i], _length[i]);
            l += _length[i];
        }
    }
    str[total_length] = '\0';

    load_shader(shader, str, total_length);

    free(str);
}

void glCompileShader_soloader(GLuint shader) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glCompileShader<%p>(shader: %i)\n", __builtin_return_address(0), shader);
#endif

#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        glCompileShader(shader);
#ifdef DUMP_COMPILED_SHADERS
        void *bin = vglMalloc(32 * 1024);
        GLsizei len;
        vglGetShaderBinary(shader, 32 * 1024, &len, bin);
        file_save(next_shader_fname, bin, len);
        vglFree(bin);
#endif
    }
    skip_next_compile = GL_FALSE;
#endif
}

#if defined(USE_GLSL_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else {
        glShaderSource(shader, 1, &string, &length);
        strcpy(next_shader_fname, gxp_path);
    }

    free(sha_name);
}
#elif defined(USE_GLSL_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    glShaderSource(shader, 1, &string, &length);
}
#elif defined(USE_CG_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    char cg_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);
    snprintf(cg_path, sizeof(cg_path), DATA_PATH"cg/%s.cg", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else if (file_exists(cg_path)) {
        char *buffer;
        size_t size;

        file_load(cg_path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);
        strcpy(next_shader_fname, gxp_path);

        free(buffer);
        skip_next_compile = GL_FALSE;
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }

        skip_next_compile = GL_FALSE;
    }

    free(sha_name);
}
#elif defined(USE_CG_SHADERS) || defined(USE_GXP_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char path[256];
#ifdef USE_CG_SHADERS
    snprintf(path, sizeof(path), DATA_PATH"cg/%s.cg", sha_name);
#else
    snprintf(path, sizeof(path), DATA_PATH"gxp/%s.gxp", sha_name);
#endif

    if (file_exists(path)) {
#ifdef USE_CG_SHADERS
        char *buffer;
        size_t size;

        file_load(path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);

        free(buffer);
#else
        uint8_t *buffer;
        size_t size;

        file_load(path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
#endif
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }
    }

    free(sha_name);
}
#else
#error "Define one of (USE_GLSL_SHADERS, USE_CG_SHADERS, USE_GXP_SHADERS)"
#endif
