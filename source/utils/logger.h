/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 * Copyright (C) 2026      Asphalt-5-Vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  logger.h
 * @brief Logging utilities.
 *
 * Every log line goes to up to three sinks at once:
 *
 *  - the debug console, via `sceClibPrintf()` (colourised);
 *  - an incremental log file on `ux0:`, one per run (see `log_init()`);
 *  - a UDP receiver, when configured (see `netlog.h`).
 *
 * File and UDP sinks are written unbuffered and plain (no ANSI escapes), so
 * the tail of the log survives a hard data abort -- which is the whole point
 * of having them during a port.
 */

#ifndef SOLOADER_LOGGER_H
#define SOLOADER_LOGGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LT_DEBUG   0
#define LT_INFO    1
#define LT_WARN    2
#define LT_ERROR   3
#define LT_FATAL   4
#define LT_SUCCESS 5
#define LT_WAIT    6

/*
 * The l_* helpers below are compiled in when the loader is built in Debug
 * (DEBUG_SOLOADER) *or* when the on-device log is enabled (ENABLE_FILE_LOG):
 * compiling them out in a release build would leave the incremental log file
 * empty, which defeats its purpose. Volume is instead controlled at runtime
 * via log_set_min_level(), so a release build can ship with the log on and
 * still not pay for per-read LT_DEBUG spam from the io reimplementation.
 */
#if defined(DEBUG_SOLOADER) || defined(ENABLE_FILE_LOG)
#define l_debug(...)   _log_print(LT_DEBUG,   __VA_ARGS__)
#define l_info(...)    _log_print(LT_INFO,    __VA_ARGS__)
#define l_warn(...)    _log_print(LT_WARN,    __VA_ARGS__)
#define l_success(...) _log_print(LT_SUCCESS, __VA_ARGS__)
#define l_wait(...)    _log_print(LT_WAIT,    __VA_ARGS__)
#else
#define l_debug(...)
#define l_info(...)
#define l_warn(...)
#define l_success(...)
#define l_wait(...)
#endif

#define l_error(...)   _log_print(LT_ERROR,   __VA_ARGS__)
#define l_fatal(...)   _log_print(LT_FATAL,   __VA_ARGS__)

void _log_print(int t, const char* fmt, ...)
                __attribute__ ((format (printf, 2, 3)));

/**
 * Open this run's log file.
 *
 * Log files are named `asphalt5_NNN.log` under `<DATA_PATH>logs/`, where `NNN`
 * runs from `001` to `999`. Each run claims the next free slot; once `999` is
 * taken the numbering wraps back to `001` and the oldest file in the way is
 * overwritten, so the directory never grows without bound. The next index is
 * remembered in `<DATA_PATH>logs/next.idx` so the sequence keeps advancing
 * across runs even when older files have been deleted by hand.
 *
 * Should be called as early as possible -- before anything that logs -- so the
 * file captures the whole run. Calling it twice is a no-op.
 *
 * @return The claimed index (`1`-`999`), or `-1` if no file could be opened.
 *         A failure here is not fatal: console and UDP sinks still work.
 */
int log_init(void);

/** Close this run's log file. */
void log_shutdown(void);

/**
 * Drop log lines below @p level.
 *
 * Defaults to `LT_DEBUG` in Debug builds and `LT_INFO` otherwise. `LT_ERROR`,
 * `LT_FATAL`, `LT_SUCCESS` and `LT_WAIT` are always emitted regardless of the
 * threshold -- they are low-volume and are exactly what a crash report needs.
 *
 * @param[in] level One of the `LT_*` constants.
 */
void log_set_min_level(int level);

/** @return This run's log index (`1`-`999`), or `-1` if the file sink is off. */
int log_current_index(void);

/** @return Full path of this run's log file, or `"(none)"`. */
const char * log_current_path(void);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_LOGGER_H
