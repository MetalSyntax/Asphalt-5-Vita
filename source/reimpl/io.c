/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <psp2/kernel/threadmgr.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

FILE * fopen_soloader(const char * filename, const char * mode) {
    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    }

    if (strstr(filename, "replay.sav") != NULL) {
        l_info("BLOCKING replay.sav to save FPS!");
        return (FILE *)0xDEADBEEF;
    }

    const char* target_filename = filename;
    char mapped_path[256];
    const char* prefix = "/data/data/com.gameloft.android.GAND.GloftA5HD/";
    if (strncmp(filename, prefix, strlen(prefix)) == 0) {
        snprintf(mapped_path, sizeof(mapped_path), "ux0:data/asphalt5/%s", filename + strlen(prefix));
        target_filename = mapped_path;
    }

#ifdef USE_SCELIBC_IO
    FILE* ret = sceLibcBridge_fopen(target_filename, mode);
#else
    FILE* ret = fopen(target_filename, mode);
#endif

    if (ret)
        l_debug("fopen(%s, %s): %p", filename, mode, ret);
    else
        l_warn("fopen(%s, %s): %p", filename, mode, ret);

    return ret;
}

int open_soloader(const char * path, int oflag, ...) {
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    }

    if (strstr(path, "replay.sav") != NULL) {
        l_info("BLOCKING replay.sav in open to save FPS!");
        return -1;
    }

    const char* target_path = path;
    char mapped_path[256];
    const char* prefix = "/data/data/com.gameloft.android.GAND.GloftA5HD/";
    if (strncmp(path, prefix, strlen(prefix)) == 0) {
        snprintf(mapped_path, sizeof(mapped_path), "ux0:data/asphalt5/%s", path + strlen(prefix));
        target_path = mapped_path;
    }

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(target_path, oflag, mode);
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    const char* target_path = path;
    char mapped_path[256];
    const char* prefix = "/data/data/com.gameloft.android.GAND.GloftA5HD/";
    if (strncmp(path, prefix, strlen(prefix)) == 0) {
        snprintf(mapped_path, sizeof(mapped_path), "ux0:data/asphalt5/%s", path + strlen(prefix));
        target_path = mapped_path;
    }

    struct stat st;
    int res = stat(target_path, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("stat(%s): %i", target_path, res);
    return res;
}

int fclose_soloader(FILE * f) {
    if (f == (FILE *)0xDEADBEEF) {
        return 0;
    }
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fclose(f);
#else
    int ret = fclose(f);
#endif

    l_debug("fclose(%p): %i", f, ret);
    return ret;
}

int close_soloader(int fd) {
    int ret = close(fd);
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    const char* target_path = _pathname;
    char mapped_path[256];
    const char* prefix = "/data/data/com.gameloft.android.GAND.GloftA5HD/";
    if (strncmp(_pathname, prefix, strlen(prefix)) == 0) {
        snprintf(mapped_path, sizeof(mapped_path), "ux0:data/asphalt5/%s", _pathname + strlen(prefix));
        target_path = mapped_path;
    }

    DIR* ret = opendir(target_path);
    l_debug("opendir(\"%s\"): %p", target_path, ret);
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

#define DUMMY_FILE_PTR ((FILE *)0xDEADBEEF)

size_t fwrite_soloader(const void * ptr, size_t size, size_t count, FILE * stream) {
    if (stream == DUMMY_FILE_PTR) {
        return count; // Pretend we wrote it all successfully
    }
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fwrite(ptr, size, count, stream);
#else
    return fwrite(ptr, size, count, stream);
#endif
}

size_t fread_soloader(void * ptr, size_t size, size_t count, FILE * stream) {
    if (stream == DUMMY_FILE_PTR) {
        return 0; // EOF
    }
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fread(ptr, size, count, stream);
#else
    return fread(ptr, size, count, stream);
#endif
}

long ftell_soloader(FILE * stream) {
    if (stream == DUMMY_FILE_PTR) {
        return 0;
    }
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_ftell(stream);
#else
    return ftell(stream);
#endif
}

int fseek_soloader(FILE * stream, long offset, int whence) {
    if (stream == DUMMY_FILE_PTR) {
        return 0;
    }
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fseek(stream, offset, whence);
#else
    return fseek(stream, offset, whence);
#endif
}

int fflush_soloader(FILE * stream) {
    if (stream == DUMMY_FILE_PTR) {
        return 0;
    }
    // Vita SDK doesn't always have a bridge for fflush. We can just use standard fflush.
    return fflush(stream);
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}
