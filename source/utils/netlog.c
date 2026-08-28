/*
 * Copyright (C) 2026 Asphalt-5-Vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/netlog.h"

#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#define NETLOG_CONFIG_PATH DATA_PATH"netlog.txt"

// SceNet wants its own pool. 16 KiB is the smallest the kernel accepts and is
// plenty for a single unconnected datagram socket.
#define NETLOG_POOL_SIZE (16 * 1024)

#ifndef NETLOG_IP
#define NETLOG_IP ""
#endif

static char _pool[NETLOG_POOL_SIZE] __attribute__((aligned(4)));

static int  _sock = -1;
static bool _tried = false;
static SceNetSockaddrIn _dst;
static char _target[64] = "(disabled)";

/**
 * Read the receiver address out of NETLOG_CONFIG_PATH.
 *
 * Uses raw sceIo rather than stdio: netlog_init() runs before FIOS and before
 * the .so's own libc reimplementation is wired up.
 */
static bool read_config(char * ip, unsigned int ip_size, unsigned short * port) {
    ip[0] = '\0';
    *port = NETLOG_DEFAULT_PORT;

    SceUID fd = sceIoOpen(NETLOG_CONFIG_PATH, SCE_O_RDONLY, 0777);
    if (fd < 0) {
        // No config file: fall back to the build-time default, if any.
        sceClibStrncpy(ip, NETLOG_IP, ip_size - 1);
        ip[ip_size - 1] = '\0';
        return ip[0] != '\0';
    }

    // Sized to comfortably hold a commented config file; anything past this
    // is ignored, so keep `ip` near the top if you write your own.
    char buf[2048];
    int read = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);

    if (read <= 0)
        return false;
    buf[read] = '\0';

    // Walk the file line by line. sscanf() is deliberately avoided here for
    // the same reason as stdio above. Any line whose first token is not a key
    // we know is skipped, which is what lets `#` comments through unharmed.
    char * line = buf;
    while (line && *line) {
        char * eol = sceClibStrchr(line, '\n');
        if (eol)
            *eol = '\0';

        // Trim a trailing CR so a CRLF file edited on Windows still parses.
        unsigned int len = sceClibStrnlen(line, 128);
        if (len > 0 && line[len - 1] == '\r')
            line[--len] = '\0';

        char * value = sceClibStrchr(line, ' ');
        if (value) {
            *value++ = '\0';
            while (*value == ' ')
                value++;

            if (sceClibStrcmp(line, "ip") == 0) {
                sceClibStrncpy(ip, value, ip_size - 1);
                ip[ip_size - 1] = '\0';
            } else if (sceClibStrcmp(line, "port") == 0) {
                int p = 0;
                for (const char * c = value; *c >= '0' && *c <= '9'; ++c)
                    p = p * 10 + (*c - '0');
                if (p > 0 && p <= 65535)
                    *port = (unsigned short)p;
            }
        }

        line = eol ? eol + 1 : NULL;
    }

    return ip[0] != '\0';
}

bool netlog_init(void) {
    if (_tried)
        return _sock >= 0;
    _tried = true;

    char ip[64];
    unsigned short port;
    if (!read_config(ip, sizeof(ip), &port))
        return false;

    // The NET module is not resident in every app context, so make sure it is
    // loaded before touching sceNet*. Already-loaded is not an error.
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

    SceNetInitParam param;
    param.memory = _pool;
    param.size   = sizeof(_pool);
    param.flags  = 0;

    // EBUSY means something already brought the stack up (a plugin, or the
    // shell); that is fine, we just must not tear it down on shutdown.
    int ret = sceNetInit(&param);
    if (ret < 0 && (unsigned int)ret != SCE_NET_ERROR_EBUSY)
        return false;

    sceNetCtlInit();

    _sock = sceNetSocket("asphalt5_netlog", SCE_NET_AF_INET,
                         SCE_NET_SOCK_DGRAM, SCE_NET_IPPROTO_UDP);
    if (_sock < 0) {
        _sock = -1;
        return false;
    }

    sceClibMemset(&_dst, 0, sizeof(_dst));
    _dst.sin_len    = sizeof(_dst);
    _dst.sin_family = SCE_NET_AF_INET;
    _dst.sin_port   = sceNetHtons(port);

    if (sceNetInetPton(SCE_NET_AF_INET, ip, &_dst.sin_addr) <= 0) {
        sceNetSocketClose(_sock);
        _sock = -1;
        return false;
    }

    sceClibSnprintf(_target, sizeof(_target), "%s:%u", ip, (unsigned int)port);
    return true;
}

void netlog_send(const char * msg, unsigned int len) {
    if (_sock < 0 || !msg || len == 0)
        return;

    // SCE_NET_MSG_DONTWAIT: a stalled or absent receiver must never slow the
    // game thread down. A dropped datagram is an acceptable trade here.
    sceNetSendto(_sock, msg, len, SCE_NET_MSG_DONTWAIT,
                 (SceNetSockaddr *)&_dst, sizeof(_dst));
}

void netlog_shutdown(void) {
    if (_sock >= 0) {
        sceNetSocketClose(_sock);
        _sock = -1;
    }
    sceClibStrncpy(_target, "(disabled)", sizeof(_target) - 1);
}

bool netlog_enabled(void) {
    return _sock >= 0;
}

const char * netlog_target(void) {
    return _target;
}
