/*
 * Copyright (C) 2026 Asphalt-5-Vita contributors
 */

#include "jni_resloader.h"
#include "utils/logger.h"
#include <falso_jni/FalsoJNI_ImplBridge.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RES_PATH DATA_PATH "data/"

// --- JDA Pool (For returning byte arrays to the engine) ---
#define RES_POOL_SLOTS 16
typedef struct {
    JavaDynArray * jda;
    jsize          cap;
} res_slot;

static res_slot _pool[RES_POOL_SLOTS];
static int      _pool_next = 0;

static JavaDynArray * pool_take(jsize len) {
    if (len <= 0) return NULL;
    res_slot * s = &_pool[_pool_next];
    _pool_next = (_pool_next + 1) % RES_POOL_SLOTS;

    if (!s->jda) {
        s->jda = jda_alloc(len, FIELD_TYPE_BYTE);
        if (!s->jda) return NULL;
        s->cap = len;
        return s->jda;
    }

    if (s->cap < len) {
        void * grown = realloc(s->jda->array, (size_t)len);
        if (!grown) return NULL;
        s->jda->array = grown;
        s->cap = len;
    }
    s->jda->len = len;
    return s->jda;
}

// --- RAM File Cache (To avoid hitting the SD card synchronously) ---
// REVERTED (2026-08-28): this was bumped to 128 to cover the full 113-file
// working set (see port_progress.md), but it made the engine stream assets
// fast enough to push MORE geometry per frame into vitaGL's fixed-size
// vertex pool than before -- confirmed on hardware via a GPU crash whose
// backtrace (gpu_alloc_mapped_aligned <- _glDrawElements_FixedFunctionIMPL
// <- glDrawElements) is IDENTICAL to Bug #19's crash, just triggered at a
// higher streaming throughput. Reverting to the value Bug #19 verified as
// stable rather than guessing at a bigger vertex pool without being able to
// test it on real hardware from this environment.
#define RAM_CACHE_SLOTS 128

typedef struct {
    char path[128];
    void * data;
    size_t size;
    uint32_t last_used;
} ram_cache_slot;

static ram_cache_slot _rcache[RAM_CACHE_SLOTS];
static uint32_t       _rcache_clock = 0;

static void * rcache_get(const char * path, size_t expected_size) {
    for (int i = 0; i < RAM_CACHE_SLOTS; i++) {
        if (_rcache[i].path[0] != '\0' && strcmp(_rcache[i].path, path) == 0) {
            if (_rcache[i].size == expected_size) {
                _rcache[i].last_used = ++_rcache_clock;
                return _rcache[i].data;
            }
        }
    }
    return NULL;
}

static void rcache_put(const char * path, const void * data, size_t size) {
    int victim = 0;
    // Find empty slot or oldest slot
    for (int i = 0; i < RAM_CACHE_SLOTS; i++) {
        if (_rcache[i].path[0] == '\0') {
            victim = i;
            break;
        }
        if (_rcache[i].last_used < _rcache[victim].last_used) {
            victim = i;
        }
    }

    if (_rcache[victim].data) {
        free(_rcache[victim].data);
    }

    _rcache[victim].data = malloc(size);
    if (_rcache[victim].data) {
        memcpy(_rcache[victim].data, data, size);
        _rcache[victim].size = size;
        strncpy(_rcache[victim].path, path, sizeof(_rcache[victim].path) - 1);
        _rcache[victim].path[sizeof(_rcache[victim].path) - 1] = '\0';
        _rcache[victim].last_used = ++_rcache_clock;
        l_info("resloader: Cached %s (Slot %d, Size %d)", path, victim, (int)size);
    }
}


static bool resolve_path(jobject jstr, char * out, size_t out_size) {
    if (!jstr) return false;
    JavaString * js = (JavaString *) jstr;
    if (!js->utf8 || !js->utf8->array) return false;

    const char * s = (const char *) js->utf8->array;
    if (strncmp(s, ".//", 3) == 0) s += 3;
    else if (strncmp(s, "./", 2) == 0) s += 2;

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;

    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n'))
        len--;

    if (len == 0) return false;

    size_t prefix = strlen(RES_PATH);
    if (prefix + len + 1 > out_size) return false;

    memcpy(out, RES_PATH, prefix);
    memcpy(out + prefix, s, len);
    out[prefix + len] = '\0';
    return true;
}

void resloader_shutdown(void) {
    for (int i = 0; i < RAM_CACHE_SLOTS; i++) {
        if (_rcache[i].data) {
            free(_rcache[i].data);
            _rcache[i].data = NULL;
        }
        _rcache[i].path[0] = '\0';
    }
}

jint impl_GLResLoader_getResourceLength(jmethodID id, va_list args) {
    jobject name = va_arg(args, jobject);
    char path[512];
    if (!resolve_path(name, path, sizeof(path))) return 0;

    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0) {
        return 0;
    }
    return (jint) st.st_size;
}

jobject impl_GLResLoader_getResourceFull(jmethodID id, va_list args) {
    jobject name = va_arg(args, jobject);
    char path[512];
    if (!resolve_path(name, path, sizeof(path))) return NULL;

    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0) {
        return NULL;
    }

    jsize size = (jsize) st.st_size;
    JavaDynArray * jda = pool_take(size);
    if (!jda) return NULL;

    // Check RAM cache
    void * cached_data = rcache_get(path, size);
    if (cached_data) {
        memcpy(jda->array, cached_data, size);
        return (jobject) jda;
    }

    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0777);
    if (fd < 0) return NULL;

    int got = sceIoRead(fd, jda->array, size);
    sceIoClose(fd);

    if (got < 0) return NULL;
    if (got < size) {
        memset((char *)jda->array + got, 0, (size_t)(size - got));
    }

    // Store in cache for next time
    rcache_put(path, jda->array, size);

    return (jobject) jda;
}

jobject impl_GLResLoader_getResourceBytes(jmethodID id, va_list args) {
    jobject name   = va_arg(args, jobject);
    jint    offset = va_arg(args, jint);
    jint    length = va_arg(args, jint);

    char path[512];
    if (!resolve_path(name, path, sizeof(path))) return NULL;

    if (length <= 0 || offset < 0) return NULL;

    JavaDynArray * jda = pool_take(length);
    if (!jda) return NULL;

    // We don't cache partial reads in RAM cache, just full reads.
    // If the full file is cached, we could extract the bytes, but the game
    // doesn't seem to use getResourceBytes in the race loop anyway.
    SceIoStat st;
    if (sceIoGetstat(path, &st) == 0) {
        void * cached_data = rcache_get(path, st.st_size);
        if (cached_data && offset + length <= st.st_size) {
            memcpy(jda->array, (char*)cached_data + offset, length);
            return (jobject) jda;
        }
    }

    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0777);
    if (fd < 0) return NULL;

    sceIoLseek(fd, offset, SCE_SEEK_SET);
    int got = sceIoRead(fd, jda->array, length);
    sceIoClose(fd);

    if (got < 0) return NULL;
    if (got < length)
        memset((char *)jda->array + got, 0, (size_t)(length - got));

    return (jobject) jda;
}
