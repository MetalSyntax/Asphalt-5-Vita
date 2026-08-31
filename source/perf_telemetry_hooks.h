/*
 * Copyright (C) 2026 Asphalt-5-Vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  perf_telemetry_hooks.h
 * @brief Frame-pacing, GPU-stall, and engine-phase telemetry.
 *
 * Originally scaffolded by `psvita-toolkit perf-telemetry --gen-hooks` with
 * its own UDP socket; rewired here to reuse the project's existing
 * `netlog.h` sink (see main() and the Bug #19-#22 GPU vertex pool crashes in
 * port_progress.md) instead of opening a second one -- same `ux0:data/
 * asphalt5/netlog.txt` config, same `nc -u -l 18194` receiver.
 *
 * Purpose: correlate a stall or a hard GPU crash (`.psp2dmp`) with exactly
 * which engine phase was in flight. PHASE_ENTER is sent before the phase
 * runs and PHASE_EXIT after it returns; if a crash kills the process mid-
 * phase, the last line on the receiving `nc` is the ENTER with no matching
 * EXIT -- that phase is the one that collapsed the GPU.
 */

#ifndef SOLOADER_PERF_TELEMETRY_HOOKS_H
#define SOLOADER_PERF_TELEMETRY_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

/** Wrap the render+present portion of the main loop with these two. */
void perf_telemetry_frame_begin(void);
void perf_telemetry_frame_end(void);

/**
 * Forces a full GPU pipeline stall (sceGxmFinish) to measure real GPU-
 * completion time. Real cost, not a fabricated counter -- call from a
 * profiling build only, and not necessarily every frame.
 */
struct SceGxmContext;
void perf_telemetry_gpu_sync_and_measure(struct SceGxmContext *context);

/**
 * Samples vitaGL's VGL_MEM_RAM pool (the 12MB pool set in gl_init(), the
 * exact pool `gpu_alloc_mapped_aligned` exhausts in the Bug #19/#20/#22 GPU
 * crashes) and streams free/total bytes every call. Watching this trend
 * toward zero on the receiving `nc` is the earliest warning of that specific
 * collapse, well before the crash dump.
 */
void perf_telemetry_vgl_pool_sample(void);

/** Send before calling into an engine phase you want to bracket. */
void perf_telemetry_phase_enter(const char *name);

/** Send right after that phase returns, with elapsed time in microseconds. */
void perf_telemetry_phase_exit(const char *name, unsigned long long elapsed_us);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_PERF_TELEMETRY_HOOKS_H
