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
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "input.h"
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
    (void) this_; (void) pos; (void) priority; (void) group; (void) cb;
    // Pass loop+pitch through (both were discarded before: loops played as
    // one-shots that got stolen, and the engine hum sat at base pitch).
    // vol<=0 means "use default" on this path.
    audio_play_sound(soundId, 1, vol > 0.0f ? vol : 1.0f, pitch > 0.0f ? pitch : 1.0f, loop);
    return 1;
}

static int hook_BaseSoundManager_stopAllSounds() {
    audio_stop_all();
    return 0;
}

// stop(int,int,int) and stop(int,int,int,int) are both (this, soundId,
// channel, ...) per the disassembly (soundId*24 table stride, channel
// passed on to nativeStopSound). Previously ret0: loops could never stop,
// so e.g. the engine hum kept droning after the race ended.
static int hook_BaseSoundManager_stop3(void *this_, int soundId, int channel) {
    (void) this_; (void) channel;
    audio_stop_sound(soundId, channel);
    return 0;
}

static int hook_BaseSoundManager_stop4(void *this_, int soundId, int channel, int a, int b) {
    (void) this_; (void) channel; (void) a; (void) b;
    audio_stop_sound(soundId, channel);
    return 0;
}

// isSoundPlaying(this, soundId, instance, package) -- confirmed in pseudo-C
// (BaseSoundManager::isSoundPlaying indexes `param_1 * 0x18` into the sound
// table, and SoundManager::SamplePlaying(id) loops `isSoundPlaying(id, i, pkg)`
// over i = 0..instances-1). This used to read the 2nd int (the instance
// index, always 0 on the first probe) as the sndId, so SamplePlaying(X) really
// answered "is sound #0 playing?". CCar::UpdateDrift only calls
// SampleStop(0x78) when SamplePlaying(0x78) != -1, so the looping drift skid
// kept droning after the drift ended until pause's stopAllSfx cleared it --
// and "sometimes" it did stop, whenever sound #0 happened to be live (Bug #30).
static int hook_BaseSoundManager_isSoundPlaying(void *this_, int sndId, int instance, int package) {
    (void) this_; (void) instance; (void) package;
    return audio_is_sound_playing(sndId);
}

static int hook_BaseSoundManager_ret0() {
    return 0;
}

/*
 * CGameSettings::Reset() -- called by CGameSettings::Init() on every single
 * boot, and again by Game::ResetData() whenever there's no valid data.sav to
 * load. Confirmed in decompiled pseudo-C that BOTH CGameSettings::Reset() and
 * the lazy-singleton constructor CGameSettings::CGameSettings() hardcode
 * `this+4` (ControlMode) to 1 -- Tilt-to-steer is the factory default, not
 * Touch Buttons. Every synthetic tap our own physical-control forwarding
 * (source/input.c) sends is calibrated against the fixed on-screen positions
 * of the Touch Buttons scheme (control mode 0); in any other mode those same
 * coordinates land on the wrong icon or nothing at all, which is exactly the
 * "nitro/brake buttons still visible and physical controls flaky" symptom
 * reported after a fresh install. A real save always wins: right after
 * Reset() runs at boot, CGameSettings::Load() freads `this+4` straight from
 * disk when data.sav exists and is valid, unconditionally overwriting
 * whatever we force here -- so this only changes the outcome the very first
 * time the port runs, before any save exists, never a player's own later
 * choice from the in-game options menu.
 */
static so_hook s_hook_cgamesettings_reset;
static void (* p_CGameSettings_SetControlMode)(void *, int);

static void hook_CGameSettings_Reset(void *this_) {
    int r = SO_CONTINUE(int, s_hook_cgamesettings_reset, this_);
    (void) r;
    if (p_CGameSettings_SetControlMode)
        p_CGameSettings_SetControlMode(this_, 0);
}

/*
 * Bug #29: nitro only fired while also steering.
 *
 * Game::TimerCallback() runs GamePadManager::Update() once per RENDER frame,
 * then steps the game logic (GS_Run::Update -> Scene::Update ->
 * Scene::UpdateCars) at a fixed 25 Hz -- `while (acc > 39ms)` -- so any
 * render frame shorter than 40ms can run ZERO logic steps. GS_Run::Update
 * turns the nitro rect (id 1) into GamePad::KeyboardKeyPressed(0x4000) every
 * step it's held; the next GamePadManager::Update() latches that into the
 * one-frame "pressed" edge (manager+8), and Scene::UpdateCars() only fires
 * nitro on that edge (`(pressed & 0x4000) && !(held & 4)` -> CCar flag 0x20).
 * If the frame that latched the edge runs no logic step, the next frame's
 * Update() computes edge = ~(edge|held) & pressed = 0 (the key is already
 * "held") and the press is gone for good -- the engine assumed Android's
 * <=25fps, the Vita renders faster.
 *
 * Steering hid it: GS_Run's steering-arrow rects (ids 6/7) go through
 * CKeyQueue::AddKeyToQueue(), which calls GamePadManager::Update() itself
 * mid-step, AFTER the nitro rect was processed and BEFORE Scene::Update --
 * latching the nitro edge inside a step that is guaranteed to reach
 * UpdateCars. Same for the brake (SQUARE) path.
 *
 * Fix: keep the nitro edge alive across GamePadManager::Update() calls until
 * a Scene::UpdateCars() actually runs, only while a race is on screen (menus
 * never run UpdateCars and must not see repeated edges). Capped so a GS_Run
 * phase that never updates cars can't hold it indefinitely.
 */
#define GAMEPAD_KEY_NITRO 0x4000
#define NITRO_EDGE_MAX_CARRY 8

static so_hook s_hook_gamepadmgr_update;
static so_hook s_hook_scene_update_cars;
static bool s_cars_updated = false;
static uint32_t s_nitro_edge = 0;
static int s_nitro_carries = 0;

static int hook_GamePadManager_Update(void *this_) {
    int r = SO_CONTINUE(int, s_hook_gamepadmgr_update, this_);
    // GamePadManager: +4 GamePad*, +8 pressed edge, +0xc released, +0x10 held
    // (GamePadManager::UpdateKeysState, confirmed in the disassembly).
    uint32_t *pressed = (uint32_t *) ((uintptr_t) this_ + 8);
    if (s_cars_updated || !input_in_race()) {
        s_nitro_carries = 0;
    } else if (s_nitro_edge && s_nitro_carries < NITRO_EDGE_MAX_CARRY) {
        *pressed |= s_nitro_edge;
        s_nitro_carries++;
    }
    s_nitro_edge = *pressed & GAMEPAD_KEY_NITRO;
    s_cars_updated = false;
    return r;
}

static int hook_Scene_UpdateCars(void *this_) {
#ifdef ENABLE_PERF_TELEMETRY
    perf_telemetry_phase_enter("Scene::UpdateCars");
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
#endif
    int r = SO_CONTINUE(int, s_hook_scene_update_cars, this_);
#ifdef ENABLE_PERF_TELEMETRY
    perf_telemetry_phase_exit("Scene::UpdateCars", sceKernelGetProcessTimeWide() - t0);
#endif
    s_cars_updated = true;
    return r;
}

/*
 * Brake + nitro on-screen buttons drawn at ~1% opacity (user request): the
 * physical buttons drive them, so the icons only cover the road. Their touch
 * rects are untouched -- only the vertex alpha of those sprites changes, and
 * every other HUD element (pause, arrows, wheel...) is drawn as before.
 *
 * GS_Run::Render() (pseudo-C) first calls Scene::Render(), then paints its
 * buttons with Sprite::PaintFrame(spr, frame, (int)rect.x0, (int)rect.y0, 0)
 * -- nitro is gxMenu item/rect id 1 (the sprite that swaps 0x1a1a/0x1b19 with
 * the nitro charge), brake is rect id 3 (bottom right) and id 9 (bottom left),
 * both painted with item 3's frames. PaintFrame -> PaintFModule ->
 * PaintModule -> Lib3D::paint2DModule() appends one quad (6 verts, RGBA8
 * colors at Lib3D+0x12d8, 0x18 bytes/quad, count at Lib3D+0x12cc) to the 2D
 * batch. So: while inside GS_Run::Render() but outside Scene::Render(), a
 * PaintFrame landing exactly on one of those rects' origin gets its freshly
 * appended quads' alpha scaled down. Flush2D() first so the batch can't
 * wrap (flush at 0x80 quads) in the middle of the button.
 *
 * PaintFrame is reimplemented here (same loop as the original,
 * .so+0x77dec) rather than SO_CONTINUE'd -- it runs dozens of times per
 * frame and SO_CONTINUE costs two kernel memcpy+flushes per call.
 */
#define DIM_BUTTON_ALPHA 3 // of 255, ~1%
#define DIM_MAX_RECTS 3

typedef void (* fn_PaintFModule)(void *spr, int frame, int module, int x, int y,
                                  unsigned flags, int a, int b, int c);
typedef void (* fn_Flush2D)(void *lib3d);

static fn_PaintFModule p_Sprite_PaintFModule;
static fn_Flush2D p_Lib3D_Flush2D;
static so_hook s_hook_gsrun_render;
static so_hook s_hook_scene_render;
static bool s_in_gsrun_render = false;
static bool s_in_scene_render = false;
static int s_dim_x[DIM_MAX_RECTS], s_dim_y[DIM_MAX_RECTS];
static int s_dim_count = 0;

static bool is_dim_button_origin(int x, int y) {
    for (int i = 0; i < s_dim_count; i++)
        if (s_dim_x[i] == x && s_dim_y[i] == y)
            return true;
    return false;
}

static void hook_Sprite_PaintFrame(void *spr, int frame, int x, unsigned y, int flags, int extra) {
    if (frame < 0)
        return;
    int modules = (*(uint8_t **) ((uintptr_t) spr + 0x24))[frame];
    if (modules == 0)
        return;

    bool dim = s_in_gsrun_render && !s_in_scene_render && p_Lib3D_Flush2D
            && !input_touch_buttons_visible() && is_dim_button_origin(x, (int) y);
    uint8_t *lib3d = *(uint8_t **) ((uintptr_t) spr + 0x70);
    int first = 0;
    if (dim) {
        p_Lib3D_Flush2D(lib3d);
        first = *(int *) (lib3d + 0x12cc);
    }

    for (int i = 0; i < modules; i++)
        p_Sprite_PaintFModule(spr, frame, i, x, (int) y, (unsigned) flags, 0, 0, extra + 1);

    if (dim) {
        int last = *(int *) (lib3d + 0x12cc);
        uint8_t *colors = *(uint8_t **) (lib3d + 0x12d8);
        for (int q = first; q < last; q++)
            for (int v = 0; v < 6; v++) {
                uint8_t *a = &colors[q * 0x18 + v * 4 + 3];
                *a = (uint8_t) ((*a * DIM_BUTTON_ALPHA) / 255);
            }
    }
}

static int hook_GS_Run_Render(void *this_) {
    // gxGameState: +0x10 RectEntry*[] , +0x18 count; RectEntry: float x0,y0
    // at +0/+4, id at +0x24 (gxGameState::FindRect / AddRectangle).
    s_dim_count = 0;
    int n = *(int *) ((uintptr_t) this_ + 0x18);
    float **rects = *(float ***) ((uintptr_t) this_ + 0x10);
    for (int i = 0; rects && i < n && s_dim_count < DIM_MAX_RECTS; i++) {
        int id = *(int *) ((uintptr_t) rects[i] + 0x24);
        if (id == 1 || id == 3 || id == 9) {
            s_dim_x[s_dim_count] = (int) rects[i][0];
            s_dim_y[s_dim_count] = (int) rects[i][1];
            s_dim_count++;
        }
    }
    s_in_gsrun_render = true;
    int r = SO_CONTINUE(int, s_hook_gsrun_render, this_);
    s_in_gsrun_render = false;
    return r;
}

static int hook_Scene_Render(void *this_) {
    s_in_scene_render = true;
#ifdef ENABLE_PERF_TELEMETRY
    perf_telemetry_phase_enter("Scene::Render");
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
#endif
    int r = SO_CONTINUE(int, s_hook_scene_render, this_);
#ifdef ENABLE_PERF_TELEMETRY
    perf_telemetry_phase_exit("Scene::Render", sceKernelGetProcessTimeWide() - t0);
#endif
    s_in_scene_render = false;
    return r;
}

/*
 * Scenery-object ambient loops (raw_157/raw_163 "revving + skid") --
 * see audio_set_ambient_scope() in audio.cpp for the falloff reshaping.
 */
static so_hook s_hook_scene_anim_sounds;

static int hook_Scene_UpdateAnimatedObjectsSounds(void *this_) {
    audio_set_ambient_scope(1);
    int r = SO_CONTINUE(int, s_hook_scene_anim_sounds, this_);
    audio_set_ambient_scope(0);
    return r;
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
static so_hook s_hook_scene_render_interface;

static int hook_Scene_Update(void *this_) {
    perf_telemetry_phase_enter("Scene::Update");
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    int r = SO_CONTINUE(int, s_hook_scene_update, this_);
    perf_telemetry_phase_exit("Scene::Update", sceKernelGetProcessTimeWide() - t0);
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
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager4stopEiii"), (uintptr_t) &hook_BaseSoundManager_stop3);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager4stopEiiii"), (uintptr_t) &hook_BaseSoundManager_stop4);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager10stopAllSfxEi"), (uintptr_t) &hook_BaseSoundManager_stopAllSounds);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager13stopAllSoundsEv"), (uintptr_t) &hook_BaseSoundManager_stopAllSounds);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager13stopAllMusicsEi"), (uintptr_t) &hook_BaseSoundManager_stopAllSounds);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager19stopAllSecondMusicsEv"), (uintptr_t) &hook_BaseSoundManager_stopAllSounds);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager6updateEi"), (uintptr_t) &hook_BaseSoundManager_ret0);
    hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN16BaseSoundManager14isSoundPlayingEiii"), (uintptr_t) &hook_BaseSoundManager_isSoundPlaying);

    // Default to Touch Buttons (control mode 0) instead of the engine's own
    // Tilt-to-steer factory default -- see the comment above
    // hook_CGameSettings_Reset() for why.
    p_CGameSettings_SetControlMode = (void (*)(void *, int))
            so_symbol(&so_mod, "_ZN13CGameSettings14SetControlModeEi");
    s_hook_cgamesettings_reset = hook_addr(
            (uintptr_t) so_symbol(&so_mod, "_ZN13CGameSettings5ResetEv"), (uintptr_t) &hook_CGameSettings_Reset);

    // Bug #29: keep the nitro press edge alive until Scene::UpdateCars()
    // consumes it -- see hook_GamePadManager_Update(). The UpdateCars hook
    // doubles as the Scene::UpdateCars telemetry phase when that's enabled.
    s_hook_gamepadmgr_update = hook_addr(
            (uintptr_t) so_symbol(&so_mod, "_ZN14GamePadManager6UpdateEv"), (uintptr_t) &hook_GamePadManager_Update);
    s_hook_scene_update_cars = hook_addr(
            (uintptr_t) so_symbol(&so_mod, "_ZN5Scene10UpdateCarsEv"), (uintptr_t) &hook_Scene_UpdateCars);

    s_hook_scene_anim_sounds = hook_addr(
            (uintptr_t) so_symbol(&so_mod, "_ZN5Scene27UpdateAnimatedObjectsSoundsEv"),
            (uintptr_t) &hook_Scene_UpdateAnimatedObjectsSounds);

    // Brake/nitro buttons at ~1% opacity -- see hook_Sprite_PaintFrame().
    p_Sprite_PaintFModule = (fn_PaintFModule) so_symbol(&so_mod, "_ZN6Sprite12PaintFModuleEiiiijiii");
    p_Lib3D_Flush2D = (fn_Flush2D) so_symbol(&so_mod, "_ZN5Lib3D7Flush2DEv");
    if (p_Sprite_PaintFModule)
        hook_addr((uintptr_t) so_symbol(&so_mod, "_ZN6Sprite10PaintFrameEiiiji"), (uintptr_t) &hook_Sprite_PaintFrame);
    s_hook_gsrun_render = hook_addr(
            (uintptr_t) so_symbol(&so_mod, "_ZN6GS_Run6RenderEv"), (uintptr_t) &hook_GS_Run_Render);
    s_hook_scene_render = hook_addr(
            (uintptr_t) so_symbol(&so_mod, "_ZN5Scene6RenderEv"), (uintptr_t) &hook_Scene_Render);

#ifdef ENABLE_PERF_TELEMETRY
    // Was previously missing entirely -- hook_Scene_* were defined above but
    // never installed, so Scene::Update()/UpdateCars()/Render()/
    // RenderInterface() ran unhooked and no PHASE_ENTER/PHASE_EXIT ever fired.
    s_hook_scene_update = hook_addr(
            (uintptr_t) so_symbol(&so_mod, "_ZN5Scene6UpdateEv"), (uintptr_t) &hook_Scene_Update);
    s_hook_scene_render_interface = hook_addr(
            (uintptr_t) so_symbol(&so_mod, "_ZN5Scene15RenderInterfaceEv"), (uintptr_t) &hook_Scene_RenderInterface);
    s_hook_rendergroups = hook_addr(
            (uintptr_t) so_symbol(&so_mod, "_ZN13gxRenderGroup12RenderGroupsEib"), (uintptr_t) &hook_gxRenderGroup_RenderGroups);
    s_hook_scene_rendercars = hook_addr(
            (uintptr_t) so_symbol(&so_mod, "_ZN5Scene10RenderCarsEh"), (uintptr_t) &hook_Scene_RenderCars);
#endif
}
