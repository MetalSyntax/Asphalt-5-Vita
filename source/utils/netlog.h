/*
 * Copyright (C) 2026 Asphalt-5-Vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  netlog.h
 * @brief Optional UDP log sink, wire-compatible with `debugnet`.
 *
 * The Vita has no usable stdout unless the app is launched from a host tool,
 * and a `.psp2dmp` only ever shows the crashed frame. Mirroring the loader log
 * over UDP gives a live view of the last lines before a hard lock-up, which a
 * file on `ux0:` cannot guarantee.
 *
 * Receiving side (any machine on the same LAN):
 * @code
 *   nc -u -l -p 18194        # Linux
 *   nc -u -l 18194           # macOS / BSD
 * @endcode
 *
 * The sink is opt-in and never fatal: if the config file is missing, the
 * network is down, or the socket cannot be created, logging silently carries
 * on through the console and the file sink.
 */

#ifndef SOLOADER_NETLOG_H
#define SOLOADER_NETLOG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Port used by `debugnet` by default. Overridable at build time. */
#ifndef NETLOG_DEFAULT_PORT
#define NETLOG_DEFAULT_PORT 18194
#endif

/**
 * Bring up the UDP sink.
 *
 * Reads `<DATA_PATH>netlog.txt` for the receiver address. The file is a
 * plain `key value` list; unknown keys are ignored:
 *
 * @code
 *   ip   192.168.3.10
 *   port 18194
 * @endcode
 *
 * If the file does not exist, `NETLOG_IP` (set at build time) is used as the
 * fallback address. If neither yields an address, the sink stays disabled.
 *
 * Safe to call more than once; only the first call does any work.
 *
 * @return `true` if the sink is up, `false` if it stayed disabled.
 */
bool netlog_init(void);

/**
 * Send one already-formatted line to the receiver.
 *
 * A no-op when the sink is disabled. Never blocks on a full send buffer:
 * datagrams are sent non-blocking and dropped rather than stalling the
 * caller, because this runs on the game thread.
 *
 * @param[in] msg Null-terminated line to send.
 * @param[in] len Length of @p msg in bytes, excluding the terminator.
 */
void netlog_send(const char * msg, unsigned int len);

/** Close the socket. Safe to call when the sink was never enabled. */
void netlog_shutdown(void);

/** @return `true` while the sink is up. */
bool netlog_enabled(void);

/** @return Receiver address as `ip:port`, or `"(disabled)"`. */
const char * netlog_target(void);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_NETLOG_H
