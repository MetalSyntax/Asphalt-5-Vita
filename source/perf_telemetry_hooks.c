/*
 * Copyright (C) 2026 Asphalt-5-Vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "perf_telemetry_hooks.h"

#include "utils/netlog.h"

#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <string.h>
#include <vitaGL.h>

/* sceKernelGetProcessTimeWide() returns the tick count directly (SceUInt64),
 * it does not take an out-pointer -- the toolkit's first draft assumed the
 * sceKernelGetProcessTime(SceKernelSysClock*) shape instead. */
static SceUInt64 s_frame_start;

/* netlog_send() sends exactly the bytes given, no framing of its own -- every
 * other caller (logger.c) appends '\n' itself, which is what makes lines show
 * up one-per-line on the `nc -u -l` receiver instead of concatenated into one
 * unbroken blob (confirmed: that's exactly what happened before this fix). */
static void pt_send(const char *line) {
    char buf[96];
    int len = snprintf(buf, sizeof(buf), "%s\n", line);
    netlog_send(buf, (unsigned int) len);
}

void perf_telemetry_frame_begin(void) {
    s_frame_start = sceKernelGetProcessTimeWide();
}

void perf_telemetry_frame_end(void) {
    SceUInt64 now = sceKernelGetProcessTimeWide();
    char line[64];
    snprintf(line, sizeof(line), "FRAME,%llu", (unsigned long long)(now - s_frame_start));
    pt_send(line);
}

/* Real GPU-completion measurement, NOT a fabricated counter: sceGxmFinish() forces
 * a full pipeline flush, so timing it measures actual GPU (plus submission) work --
 * but that stall is a real cost. Call this from a profiling build only, and not
 * necessarily every frame (e.g. every 60th) -- never in anything you'd ship. */
void perf_telemetry_gpu_sync_and_measure(struct SceGxmContext *context) {
    SceUInt64 start = sceKernelGetProcessTimeWide();
    sceGxmFinish((SceGxmContext *) context);
    SceUInt64 end = sceKernelGetProcessTimeWide();
    char line[64];
    snprintf(line, sizeof(line), "GPU,%llu", (unsigned long long)(end - start));
    pt_send(line);
}

/* VGL_MEM_RAM is the 12MB ram_threshold pool passed to vglInitExtended() in
 * gl_init() -- the exact pool gpu_alloc_mapped_aligned() exhausts in the
 * confirmed Bug #19/#20/#22 GPU crashes (glDrawElements queuing more geometry
 * per frame than the pool holds). Free bytes trending to 0 right before a
 * crash confirms the same mechanism is recurring; a healthy floor rules it
 * out and points back at Scene::Render/RenderInterface's draw call count or
 * CPU-side stalls instead. */
void perf_telemetry_vgl_pool_sample(void) {
    // %zu is not supported by this VITASDK's snprintf (confirmed on hardware:
    // it prints the literal conversion "zu", not the value) -- cast to
    // unsigned long long and use %llu, same as FRAME/GPU above.
    unsigned long long free_bytes = (unsigned long long) vglMemFree(VGL_MEM_RAM);
    unsigned long long total_bytes = (unsigned long long) vglMemTotal(VGL_MEM_RAM);
    char line[64];
    snprintf(line, sizeof(line), "VGLPOOL,%llu,%llu", free_bytes, total_bytes);
    pt_send(line);
}

void perf_telemetry_phase_enter(const char *name) {
    char line[80];
    snprintf(line, sizeof(line), "PHASE_ENTER,%s", name);
    pt_send(line);
}

void perf_telemetry_phase_exit(const char *name, unsigned long long elapsed_us) {
    char line[80];
    snprintf(line, sizeof(line), "PHASE_EXIT,%s,%llu", name, elapsed_us);
    pt_send(line);
}
