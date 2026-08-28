/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 */

#include <kubridge.h>
#include <so_util/so_util.h>

#include <string.h>

extern so_module so_mod;

/*
 * CMatrix::Mult/PreMult/SetMult use the legacy VFPv2 "short vector" trick
 * (FPSCR.Len=4, Stride=1: a scalar-times-vector `vmul`/`vmla` chain computing
 * 2 output rows per pass) to do 4x4 matrix multiplication in hardware. That
 * mode was dropped starting VFPv3 and the Vita's Cortex-A9 doesn't implement
 * it at all -- the moment FPSCR.Len is nonzero and one of these `vmul.f32`/
 * `vmla.f32` executes, the CPU raises Undefined Instruction. Confirmed via
 * `objdump`: only 3 functions in the whole .so use this pattern (all matrix
 * multiplies, same 26-instruction shape, `_ZN7CMatrix4MultEPS_` /
 * `_ZN7CMatrix7PreMultEPS_` / `_ZN7CMatrix7SetMultEPS_S0_`), so they're
 * replaced wholesale with a portable scalar implementation instead of trying
 * to patch around the FPSCR setup (the actual `vmul`/`vmla` instructions rely
 * on vector-mode register-range expansion for correctness, not just as a
 * performance shortcut -- disabling vector mode without rewriting the math
 * would silently corrupt every transform instead of crashing).
 *
 * Semantics reverse-engineered from the disassembly (confirmed row-major
 * storage and index convention against `CMatrix::TransformVector`, which
 * unambiguously does `out = M * v` with `M[row][col]` at byte
 * `row*16 + col*4`): all three compute an ordinary `V * M` 4x4 product,
 * `Result[row][col] = sum_k V[row][k] * M[k][col]`. They only differ in
 * which operand plays `V` (the one read 2-rows-at-a-time through the scalar
 * bank) vs `M` (the one loaded whole into the vector bank) and where the
 * result lands:
 *
 *   Mult(other):        this  = this  * other   (V=this,  M=other, dest=this)
 *   PreMult(other):      this  = other * this    (V=other, M=this,  dest=this)
 *   SetMult(this,a,b):  this  = a     * b        (V=a,     M=b,     dest=this)
 */
typedef struct {
    float m[4][4];
} cmatrix_raw;

__attribute__((optimize("O3", "fast-math", "unroll-loops")))
static void cmatrix_mul(const cmatrix_raw * a, const cmatrix_raw * b, cmatrix_raw * out) {
    cmatrix_raw tmp; // safe even when `out` aliases `a` and/or `b`
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++)
                sum += a->m[row][k] * b->m[k][col];
            tmp.m[row][col] = sum;
        }
    }
    memcpy(out, &tmp, sizeof(tmp));
}

__attribute__((optimize("O3")))
static void hook_CMatrix_Mult(cmatrix_raw * this_, const cmatrix_raw * other) {
    cmatrix_mul(this_, other, this_);
}

__attribute__((optimize("O3")))
static void hook_CMatrix_PreMult(cmatrix_raw * this_, const cmatrix_raw * other) {
    cmatrix_mul(other, this_, this_);
}

__attribute__((optimize("O3")))
static void hook_CMatrix_SetMult(cmatrix_raw * this_, const cmatrix_raw * a, const cmatrix_raw * b) {
    cmatrix_mul(a, b, this_);
}

#include "audio.h"

static int hook_BaseSoundManager_playEx(void *this_, int soundId, const float *pos, int loop, float vol, int priority, int group, float pitch, void (*cb)()) {
    (void) this_; (void) pos; (void) loop; (void) priority; (void) group; (void) pitch; (void) cb;
    audio_play_sound(soundId, 0, vol > 0.0f ? vol : 1.0f);
    return 1;
}

static int hook_BaseSoundManager_stopAllSounds() {
    audio_stop_all();
    return 0;
}

static int hook_BaseSoundManager_ret0() {
    return 0;
}

void so_patch(void) {
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN7CMatrix4MultEPS_"),
              (uintptr_t) &hook_CMatrix_Mult);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN7CMatrix7PreMultEPS_"),
              (uintptr_t) &hook_CMatrix_PreMult);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN7CMatrix7SetMultEPS_S0_"),
              (uintptr_t) &hook_CMatrix_SetMult);
    
    // Sound Manager bridges
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager6playExEiPKfbfiifPFvvE"), (uintptr_t) &hook_BaseSoundManager_playEx);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager4stopEiiii"), (uintptr_t) &hook_BaseSoundManager_ret0);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager4stopEiii"), (uintptr_t) &hook_BaseSoundManager_ret0);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager10stopAllSfxEi"), (uintptr_t) &hook_BaseSoundManager_stopAllSounds);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager13stopAllSoundsEv"), (uintptr_t) &hook_BaseSoundManager_stopAllSounds);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager13stopAllMusicsEi"), (uintptr_t) &hook_BaseSoundManager_stopAllSounds);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager19stopAllSecondMusicsEv"), (uintptr_t) &hook_BaseSoundManager_stopAllSounds);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager6updateEi"), (uintptr_t) &hook_BaseSoundManager_ret0);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager14isSoundPlayingEiii"), (uintptr_t) &hook_BaseSoundManager_ret0);
}
