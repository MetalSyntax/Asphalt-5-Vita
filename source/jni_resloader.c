/*
 * Copyright (C) 2026 Asphalt-5-Vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "jni_resloader.h"

#include "utils/logger.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Recycled buffer pool for the byte arrays we hand back to the engine.
 *
 * FalsoJNI's ReleaseByteArrayElements() and DeleteLocalRef() are both no-ops,
 * so anything jda_alloc()'d for a return value is never freed. Allocating fresh
 * per call would leak on every read -- package_general.bar alone is 93 chunks
 * of 1 MiB. Instead we round-robin a fixed number of buffers and grow each on
 * demand, which bounds total usage to the largest few requests.
 *
 * This assumes the engine copies out of a returned array before it has made
 * RES_POOL_SLOTS further resource calls. That holds for the streaming reader in
 * Package.cpp (it memcpy's straight out of GetByteArrayElements), and the slot
 * count leaves generous headroom.
 */
#define RES_POOL_SLOTS 8

typedef struct {
    JavaDynArray * jda;
    jsize          cap;
} res_slot;

static res_slot _pool[RES_POOL_SLOTS];
static int      _pool_next = 0;

/*
 * Small LRU of open handles. A single-entry cache used to sit here, but the
 * engine streams package chunks in an interleaved pattern during real
 * gameplay (e.g. `..._019.cnk`, `..._038.cnk`, `..._019.cnk`, `..._038.cnk`,
 * ...) rather than one-file-at-a-time -- a 1-entry cache gets evicted on
 * every single call under that pattern, so every read pays a fresh
 * sceIoOpen()/sceIoClose() round trip to the memory card. That's cheap once
 * during loading, but during a race, streamed every frame, it's the
 * difference between smooth and single-digit FPS. 8 slots covers the game
 * juggling a handful of chunks/sounds at once; every read still seeks
 * explicitly, so a cached handle carries no positional state.
 */
#define RES_FD_SLOTS 8

typedef struct {
    SceUID   fd;
    char     path[256]; // empty string means the slot is free
    uint32_t last_used;
} res_fd_slot;

static res_fd_slot _fd_slots[RES_FD_SLOTS];
static uint32_t    _fd_clock = 0;

/*
 * The engine legitimately probes for optional resources, so a miss is not an
 * error -- but a *systematic* miss (wrong path, data not deployed) is the first
 * thing you want to see. Report the first few at warning level so they show up
 * without turning on debug logging, then go quiet.
 */
#define RES_MISS_REPORT_LIMIT 12
static int _miss_reported = 0;

static void report_miss(const char * path) {
    if (_miss_reported < RES_MISS_REPORT_LIMIT) {
        ++_miss_reported;
        l_warn("resloader: not found (%d/%d): %s",
               _miss_reported, RES_MISS_REPORT_LIMIT, path);
        if (_miss_reported == RES_MISS_REPORT_LIMIT)
            l_warn("resloader: further misses only at debug level. If the game "
                   "data is not under " RES_PATH ", that is the problem.");
    } else {
        l_debug("resloader: not found: %s", path);
    }
}

static JavaDynArray * pool_take(jsize len) {
    if (len <= 0)
        return NULL;

    res_slot * s = &_pool[_pool_next];
    _pool_next = (_pool_next + 1) % RES_POOL_SLOTS;

    if (!s->jda) {
        s->jda = jda_alloc(len, FIELD_TYPE_BYTE);
        if (!s->jda) {
            l_error("resloader: jda_alloc(%d) failed", (int)len);
            return NULL;
        }
        s->cap = len;
        return s->jda;
    }

    if (s->cap < len) {
        void * grown = realloc(s->jda->array, (size_t)len);
        if (!grown) {
            l_error("resloader: realloc(%d) failed", (int)len);
            return NULL;
        }
        s->jda->array = grown;
        s->cap = len;
    }

    // GetArrayLength() and the bounds checks in GetByteArrayRegion() both read
    // jda->len, so it has to track the logical size, not the capacity.
    s->jda->len = len;
    return s->jda;
}

/**
 * Turn the jstring the engine passed into a full Vita path under RES_PATH.
 *
 * Mirrors the normalisation the Java side did before touching the filesystem:
 * strip a leading `.//` or `./`, then trim surrounding whitespace.
 *
 * @return `true` on success, `false` if the string was unusable.
 */
static bool resolve_path(jobject jstr, char * out, size_t out_size) {
    if (!jstr) {
        l_error("resloader: resource name is NULL");
        return false;
    }

    JavaString * js = (JavaString *) jstr;
    if (!js->utf8 || !js->utf8->array) {
        l_error("resloader: resource name has no UTF-8 representation");
        return false;
    }

    const char * s = (const char *) js->utf8->array;

    if (strncmp(s, ".//", 3) == 0)
        s += 3;
    else if (strncmp(s, "./", 2) == 0)
        s += 2;

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;

    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'
                       || s[len - 1] == '\r' || s[len - 1] == '\n'))
        len--;

    if (len == 0) {
        l_error("resloader: resource name is empty");
        return false;
    }

    size_t prefix = strlen(RES_PATH);
    if (prefix + len + 1 > out_size) {
        l_error("resloader: path too long for '%.*s'", (int)len, s);
        return false;
    }

    memcpy(out, RES_PATH, prefix);
    memcpy(out + prefix, s, len);
    out[prefix + len] = '\0';
    return true;
}

static SceUID res_open(const char * path) {
    for (int i = 0; i < RES_FD_SLOTS; i++) {
        if (_fd_slots[i].path[0] != '\0' && strcmp(_fd_slots[i].path, path) == 0) {
            _fd_slots[i].last_used = ++_fd_clock;
            return _fd_slots[i].fd;
        }
    }

    // Pick a free slot, or evict whichever was used longest ago.
    int victim = 0;
    for (int i = 0; i < RES_FD_SLOTS; i++) {
        if (_fd_slots[i].path[0] == '\0') {
            victim = i;
            break;
        }
        if (_fd_slots[i].last_used < _fd_slots[victim].last_used)
            victim = i;
    }
    if (_fd_slots[victim].path[0] != '\0')
        sceIoClose(_fd_slots[victim].fd);

    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0777);
    if (fd < 0) {
        _fd_slots[victim].path[0] = '\0';
        return fd;
    }

    _fd_slots[victim].fd = fd;
    strncpy(_fd_slots[victim].path, path, sizeof(_fd_slots[victim].path) - 1);
    _fd_slots[victim].path[sizeof(_fd_slots[victim].path) - 1] = '\0';
    _fd_slots[victim].last_used = ++_fd_clock;
    return fd;
}

void resloader_shutdown(void) {
    for (int i = 0; i < RES_FD_SLOTS; i++) {
        if (_fd_slots[i].path[0] != '\0') {
            sceIoClose(_fd_slots[i].fd);
            _fd_slots[i].path[0] = '\0';
        }
    }
}

jint impl_GLResLoader_getResourceLength(jmethodID id, va_list args) {
    jobject name = va_arg(args, jobject);

    char path[512];
    if (!resolve_path(name, path, sizeof(path)))
        return 0;

    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0) {
        // Returning 0 for a missing file is what the Java version did.
        report_miss(path);
        return 0;
    }

    l_debug("resloader: length %s = %d", path, (int)st.st_size);
    return (jint) st.st_size;
}

jobject impl_GLResLoader_getResourceFull(jmethodID id, va_list args) {
    jobject name = va_arg(args, jobject);

    char path[512];
    if (!resolve_path(name, path, sizeof(path)))
        return NULL;

    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0) {
        l_error("resloader: getResourceFull: not found: %s", path);
        return NULL;
    }

    jsize size = (jsize) st.st_size;
    JavaDynArray * jda = pool_take(size);
    if (!jda)
        return NULL;

    SceUID fd = res_open(path);
    if (fd < 0) {
        l_error("resloader: getResourceFull: open failed (0x%08X): %s",
                (unsigned int)fd, path);
        return NULL;
    }

    sceIoLseek(fd, 0, SCE_SEEK_SET);
    int got = sceIoRead(fd, jda->array, size);
    if (got < 0) {
        l_error("resloader: getResourceFull: read failed (0x%08X): %s",
                (unsigned int)got, path);
        return NULL;
    }
    if (got < size) {
        l_warn("resloader: getResourceFull: short read %d/%d, zero-filling: %s",
               got, (int)size, path);
        memset((char *)jda->array + got, 0, (size_t)(size - got));
    }

    l_debug("resloader: full %s (%d bytes)", path, (int)size);
    return (jobject) jda;
}

jobject impl_GLResLoader_getResourceBytes(jmethodID id, va_list args) {
    jobject name   = va_arg(args, jobject);
    jint    offset = va_arg(args, jint);
    jint    length = va_arg(args, jint);

    char path[512];
    if (!resolve_path(name, path, sizeof(path)))
        return NULL;

    if (length <= 0 || offset < 0) {
        l_error("resloader: getResourceBytes: bad range offset=%d length=%d: %s",
                (int)offset, (int)length, path);
        return NULL;
    }

    // The engine expects an array of exactly `length`; a short read is
    // zero-filled, matching InputStream.read() leaving the tail untouched.
    JavaDynArray * jda = pool_take(length);
    if (!jda)
        return NULL;

    SceUID fd = res_open(path);
    if (fd < 0) {
        l_error("resloader: getResourceBytes: open failed (0x%08X): %s",
                (unsigned int)fd, path);
        return NULL;
    }

    if (sceIoLseek(fd, offset, SCE_SEEK_SET) < 0) {
        l_error("resloader: getResourceBytes: seek to %d failed: %s",
                (int)offset, path);
        return NULL;
    }

    int got = sceIoRead(fd, jda->array, length);
    if (got < 0) {
        l_error("resloader: getResourceBytes: read failed (0x%08X): %s",
                (unsigned int)got, path);
        return NULL;
    }
    if (got < length)
        memset((char *)jda->array + got, 0, (size_t)(length - got));

    l_debug("resloader: bytes %s [%d..%d)", path, (int)offset,
            (int)(offset + length));
    return (jobject) jda;
}
