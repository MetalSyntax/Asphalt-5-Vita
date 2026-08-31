/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  glutil.h
 * @brief OpenGL API initializer, related functions.
 */

#ifndef SOLOADER_GLUTIL_H
#define SOLOADER_GLUTIL_H

#include <vitaGL.h>

#ifdef __cplusplus
extern "C" {
#endif

void gl_init();

void gl_preload();

void gl_swap();

/*
 * Internal size of the offscreen downsample FBO that the engine actually
 * draws into (see gl_init()/gl_swap() in glutil.c) -- smaller than SCREEN_W/H
 * (the 800x480 the engine is TOLD the screen is, which its menu layout needs)
 * and smaller than the real physical panel (960x544). Public so video.cpp's
 * video quad -- which renders into the same permanently-bound FBO -- always
 * targets the exact same size instead of a second hardcoded copy that could
 * drift out of sync (see Bug #13 in port_progress.md for what happens when it
 * does: the video gets clipped to whatever the FBO's real attachment size is).
 * 720x432 keeps the same 5:3 aspect as 800x480 (both a clean x0.9 in each
 * axis), so glViewport_soloader()/glScissor_soloader() below can rescale the
 * engine's 800x480-space rects onto it with exact integer math, no rounding
 * seams, and no distortion.
 */
#define OFFSCREEN_W 720
#define OFFSCREEN_H 432

void glCopyTexImage2D_soloader(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
void glCopyTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);

void glCompileShader_soloader(GLuint shader);

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length);

/*
 * Rescale the engine's glViewport/glScissor rects (issued in the 800x480
 * "reported screen" space it was told about in Renderer_nativeInit()) onto
 * the real, smaller OFFSCREEN_W/H color/depth attachment size before handing
 * them to vitaGL. Unlike the reverted Bug #8 attempt, this always shrinks
 * (never enlarges) and the FBO's actual attachment IS sized to match, so
 * there's no framebuffer/viewport size mismatch to trip up GXM -- see the
 * comment above gl_init_downsample() in glutil.c.
 */
void glViewport_soloader(GLint x, GLint y, GLsizei width, GLsizei height);
void glScissor_soloader(GLint x, GLint y, GLsizei width, GLsizei height);

/*
 * Shadow copies of a handful of engine-set fixed-function states, updated
 * only by glEnable_soloader/glDisable_soloader/glEnableClientState_soloader/
 * glDisableClientState_soloader below (i.e. only by the .so's own calls,
 * routed through dynlib.c's import table) -- never by this port's own GL
 * calls, which always link the real vitaGL entry points directly instead of
 * going through the import table. Lets gl_blit_downsample_to_screen() save
 * the bits of state it clobbers without round-tripping glIsEnabled() for
 * each one every single frame. Deliberately limited to state that is NOT
 * per-texture-unit (GL_DEPTH_TEST/BLEND/SCISSOR_TEST/CULL_FACE are global,
 * and GL_VERTEX_ARRAY -- unlike GL_TEXTURE_COORD_ARRAY -- isn't affected by
 * glClientActiveTexture) so a single flag per state is always correct; the
 * texture-related bits the blit also saves (bound GL_TEXTURE_2D, its enable,
 * env mode, current color) are left as real queries since the engine does
 * use multitexturing (glActiveTexture/glClientActiveTexture are both
 * imported) and shadowing those correctly would need one slot per unit.
 */
void glEnable_soloader(GLenum cap);
void glDisable_soloader(GLenum cap);
void glEnableClientState_soloader(GLenum array);
void glDisableClientState_soloader(GLenum array);

extern GLboolean g_shadow_depth_test;
extern GLboolean g_shadow_blend;
extern GLboolean g_shadow_scissor_test;
extern GLboolean g_shadow_cull_face;
extern GLboolean g_shadow_vertex_array;

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H
