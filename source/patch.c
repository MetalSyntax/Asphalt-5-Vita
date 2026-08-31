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

#include <psp2/kernel/processmgr.h>
#include <string.h>

#include "perf_telemetry_hooks.h"

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

#ifdef ENABLE_PERF_TELEMETRY
/*
 * Diagnostic-only: bracket the 4 top-level per-frame phases confirmed in
 * decompiled/libasphalt5_armeabi/ghidra/out_ghidra.c (Scene::Update(),
 * Scene::UpdateCars(), Scene::Render(), Scene::RenderInterface() -- all
 * `(this)`-only, no other params) with PHASE_ENTER/PHASE_EXIT telemetry, to
 * find which one is running when a frame collapses or the GPU hard-crashes
 * (Bug #19/#20/#22). SO_CONTINUE (so_util.h) temporarily restores the two
 * instructions hook_addr() overwrote, calls straight into the untouched
 * function body, then re-applies the hook -- the original logic runs
 * unmodified, only timing is added around it.
 */
static so_hook s_hook_scene_update;
static so_hook s_hook_scene_update_cars;
static so_hook s_hook_scene_render;
static so_hook s_hook_scene_render_interface;

static int hook_Scene_Update(void *this_) {
    perf_telemetry_phase_enter("Scene::Update");
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    int r = SO_CONTINUE(int, s_hook_scene_update, this_);
    perf_telemetry_phase_exit("Scene::Update", sceKernelGetProcessTimeWide() - t0);
    return r;
}

static int hook_Scene_UpdateCars(void *this_) {
    perf_telemetry_phase_enter("Scene::UpdateCars");
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    int r = SO_CONTINUE(int, s_hook_scene_update_cars, this_);
    perf_telemetry_phase_exit("Scene::UpdateCars", sceKernelGetProcessTimeWide() - t0);
    return r;
}

static int hook_Scene_Render(void *this_) {
    perf_telemetry_phase_enter("Scene::Render");
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    int r = SO_CONTINUE(int, s_hook_scene_render, this_);
    perf_telemetry_phase_exit("Scene::Render", sceKernelGetProcessTimeWide() - t0);
    return r;
}

static int hook_Scene_RenderInterface(void *this_) {
    perf_telemetry_phase_enter("Scene::RenderInterface");
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    int r = SO_CONTINUE(int, s_hook_scene_render_interface, this_);
    perf_telemetry_phase_exit("Scene::RenderInterface", sceKernelGetProcessTimeWide() - t0);
    return r;
}

/*
 * Sub-phases inside Scene::Render() (see out_ghidra.c:46136), added after the
 * first console capture showed Scene::Render() itself taking 488ms then
 * 3.5s (crash) while the only other phase nested in it, RenderInterface, took
 * 11-15us -- ruling out RenderInterface and narrowing the search to whatever
 * else Render() calls directly:
 *   - gxRenderGroup::RenderGroups(group, bool) -- called ~5x per Render() for
 *     road/track geometry (opaque, reflection blend/add, transparent group
 *     passes); the single function most likely to hit vitaGL's vertex pool
 *     (Bug #19/#20/#22) since it submits the bulk of the scene's geometry.
 *   - Scene::RenderCars(uchar) -- car meshes, the other big geometry submitter.
 * Both are hooked the same SO_CONTINUE way as Scene::Render() etc. above --
 * timing added around the untouched original, no behavior change.
 */
static so_hook s_hook_rendergroups;
static so_hook s_hook_scene_rendercars;

static int hook_gxRenderGroup_RenderGroups(int group_ptr, int reflect_flag) {
    perf_telemetry_phase_enter("gxRenderGroup::RenderGroups");
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    int r = SO_CONTINUE(int, s_hook_rendergroups, group_ptr, reflect_flag);
    perf_telemetry_phase_exit("gxRenderGroup::RenderGroups", sceKernelGetProcessTimeWide() - t0);
    return r;
}

static int hook_Scene_RenderCars(void *this_, int param) {
    perf_telemetry_phase_enter("Scene::RenderCars");
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    int r = SO_CONTINUE(int, s_hook_scene_rendercars, this_, param);
    perf_telemetry_phase_exit("Scene::RenderCars", sceKernelGetProcessTimeWide() - t0);
    return r;
}
#endif // ENABLE_PERF_TELEMETRY

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

#ifdef ENABLE_PERF_TELEMETRY
    // Was previously missing entirely -- hook_Scene_* were defined above but
    // never installed, so Scene::Update()/UpdateCars()/Render()/
    // RenderInterface() ran unhooked and no PHASE_ENTER/PHASE_EXIT ever fired.
    s_hook_scene_update = hook_addr(
            (uintptr_t) so_symbol(&so_mod, "_ZN5Scene6UpdateEv"), (uintptr_t) &hook_Scene_Update);
    s_hook_scene_update_cars = hook_addr(
            (uintptr_t) so_symbol(&so_mod, "_ZN5Scene10UpdateCarsEv"), (uintptr_t) &hook_Scene_UpdateCars);
    s_hook_scene_render = hook_addr(
            (uintptr_t) so_symbol(&so_mod, "_ZN5Scene6RenderEv"), (uintptr_t) &hook_Scene_Render);
    s_hook_scene_render_interface = hook_addr(
            (uintptr_t) so_symbol(&so_mod, "_ZN5Scene15RenderInterfaceEv"), (uintptr_t) &hook_Scene_RenderInterface);
    s_hook_rendergroups = hook_addr(
            (uintptr_t) so_symbol(&so_mod, "_ZN13gxRenderGroup12RenderGroupsEib"), (uintptr_t) &hook_gxRenderGroup_RenderGroups);
    s_hook_scene_rendercars = hook_addr(
            (uintptr_t) so_symbol(&so_mod, "_ZN5Scene10RenderCarsEh"), (uintptr_t) &hook_Scene_RenderCars);
#endif
}
