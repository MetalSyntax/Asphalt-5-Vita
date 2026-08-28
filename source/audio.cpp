/**
 * @file audio.cpp
 * @brief Native sceAudioOut-based audio mixer for Asphalt 5 on PS Vita.
 */

#include "audio.h"
#include "utils/logger.h"

#define MINIMP3_IMPLEMENTATION
#include <minimp3/minimp3_ex.h>

#include <stb/stb_vorbis.c>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MIX_RATE   44100
#define MIX_GRAIN  2048
#define MAX_VOICES 16

// libasphalt5.so's Java_..._GLMediaPlayer_nativeGetTotalSounds() returns
// 0xbb (187) -- matches the 187 real raw_000.glsnd..raw_186.glsnd files
// under RES_PATH exactly. 128 silently dropped every sndId from 128 to 186.
#define MAX_SOUNDS 200

struct SfxSample {
    short *pcm;        // interleaved stereo/mono PCM
    unsigned frames;
    int channels;      // 1 or 2
    int rate;
};

struct Voice {
    SfxSample *smp;
    double pos;
    double step;
    float gain;
    float targetGain;
    int fadeFramesLeft;
    float gainStep;
    bool loop;
    bool paused;
    int sndId;
    int instance;
};

static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static bool gAudioReady = false;
static volatile int gQuit = 0;
static int gPort = -1;
static SceUID gThread = -1;

static SfxSample *gCache[MAX_SOUNDS];
static Voice gVoices[MAX_VOICES];
static Voice gBig;

/*
 * gCache[] sentinels, beyond NULL (never requested) and a real pointer
 * (decoded, ready to play):
 *   SFX_FAILED  -- resolve/decode was attempted and permanently failed.
 *   SFX_PENDING -- handed off to gLoaderThread, decode not finished yet.
 * Two distinct negative sentinels (not just "somehow NULL again") so the
 * shutdown free-loop and sfx_get() can tell "will never be ready" apart
 * from "ask again later" without a separate parallel array.
 */
#define SFX_FAILED  ((SfxSample *) -1)
#define SFX_PENDING ((SfxSample *) -2)

// Protects gCache[] specifically -- deliberately NOT the same lock as gLock
// (which guards gVoices[]/gBig), so a slow decode on gLoaderThread can never
// end up blocked behind, or blocking, the audio-critical mixer_thread's
// per-block voice mixing.
static pthread_mutex_t gCacheLock = PTHREAD_MUTEX_INITIALIZER;

#define SFX_QUEUE_CAP 32
static int gLoadQueue[SFX_QUEUE_CAP];
static int gLoadQueueHead = 0, gLoadQueueTail = 0, gLoadQueueCount = 0;
static pthread_mutex_t gQueueLock = PTHREAD_MUTEX_INITIALIZER;
static SceUID gLoaderThread = -1;
static volatile int gLoaderQuit = 0;

static bool file_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

/*
 * Confirmed against the decompiled `GLMediaPlayer.java` (`loadSound()`/
 * `loadSoundBig()`, `loadFromPack == false` path -- the one actually taken,
 * this APK has no packed `res/raw/` sound assets, see PORTING_PLAN.md):
 *
 *   if (index < 10)       sound_Name = "raw_00" + index;
 *   else if (index < 100) sound_Name = "raw_0"  + index;
 *   else                  sound_Name = "raw_"   + index;
 *   sound_Name += ".glsnd";
 *
 * i.e. always `raw_NNN.glsnd`, zero-padded to exactly 3 digits -- no other
 * naming scheme is ever used by the real app. Every real `.glsnd` file
 * checked (raw_000, raw_001, raw_010, raw_050) starts with the `OggS`
 * magic and a literal "vorbis" codec string, so `decode_ogg_file()` decodes
 * it directly; the extension is just cosmetic obfuscation, not a real
 * format difference.
 */
static bool resolve_sound_path(int index, char *outPath, size_t outSize) {
    char candidate[512];
    snprintf(candidate, sizeof(candidate), RES_PATH "raw_%03d.glsnd", index);
    if (file_exists(candidate)) {
        strncpy(outPath, candidate, outSize);
        return true;
    }
    return false;
}

static bool decode_mp3_file(const char *path, SfxSample *out) {
    mp3dec_t mp3d;
    mp3dec_file_info_t info;
    if (mp3dec_load(&mp3d, path, &info, NULL, NULL)) {
        return false;
    }
    out->pcm = info.buffer;
    out->channels = info.channels;
    out->rate = info.hz;
    out->frames = (unsigned)(info.samples / info.channels);
    return true;
}

static bool decode_ogg_file(const char *path, SfxSample *out) {
    short *pcm = NULL;
    int channels = 0, rate = 0;
    int samples = stb_vorbis_decode_filename(path, &channels, &rate, &pcm);
    if (samples <= 0 || !pcm) {
        return false;
    }
    out->pcm = pcm;
    out->channels = channels;
    out->rate = rate;
    out->frames = (unsigned) samples;
    return true;
}

static bool decode_wav_file(const char *path, SfxSample *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    unsigned char riff[12];
    if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(f);
        return false;
    }

    int channels = 0, rate = 0, bits = 0;
    bool haveFmt = false, haveData = false;
    unsigned dataSize = 0;

    while (!haveData) {
        unsigned char hdr[8];
        if (fread(hdr, 1, 8, f) != 8) break;
        unsigned chunkSize = (unsigned) hdr[4] | ((unsigned) hdr[5] << 8) | ((unsigned) hdr[6] << 16) | ((unsigned) hdr[7] << 24);

        if (memcmp(hdr, "fmt ", 4) == 0) {
            unsigned char fb[16];
            unsigned toRead = chunkSize < sizeof(fb) ? chunkSize : (unsigned) sizeof(fb);
            if (fread(fb, 1, toRead, f) != toRead) break;
            if (chunkSize > toRead) fseek(f, (long)(chunkSize - toRead), SEEK_CUR);
            channels = fb[2] | (fb[3] << 8);
            rate = fb[4] | (fb[5] << 8) | (fb[6] << 16) | (fb[7] << 24);
            bits = fb[14] | (fb[15] << 8);
            haveFmt = true;
        } else if (memcmp(hdr, "data", 4) == 0) {
            dataSize = chunkSize;
            haveData = true;
        } else {
            fseek(f, (long)(chunkSize + (chunkSize & 1)), SEEK_CUR);
        }
    }

    if (!haveFmt || !haveData || channels < 1 || channels > 2 || rate <= 0 || dataSize == 0 || bits != 16) {
        fclose(f);
        return false;
    }

    short *pcm = (short *) malloc(dataSize);
    if (!pcm) {
        fclose(f);
        return false;
    }
    if (fread(pcm, 1, dataSize, f) != dataSize) {
        free(pcm);
        fclose(f);
        return false;
    }
    fclose(f);

    out->pcm = pcm;
    out->channels = channels;
    out->rate = rate;
    out->frames = dataSize / (channels * sizeof(short));
    return true;
}

static SfxSample *sfx_load_sample(const char *path) {
    SfxSample *s = (SfxSample *) calloc(1, sizeof(SfxSample));
    if (!s) return NULL;

    const char *dot = strrchr(path, '.');
    bool ok = false;
    if (dot && strcasecmp(dot, ".mp3") == 0) {
        ok = decode_mp3_file(path, s);
    } else if (dot && strcasecmp(dot, ".ogg") == 0) {
        ok = decode_ogg_file(path, s);
    } else if (dot && strcasecmp(dot, ".wav") == 0) {
        ok = decode_wav_file(path, s);
    } else {
        // Try OGG then MP3 then WAV
        ok = decode_ogg_file(path, s);
        if (!ok) ok = decode_mp3_file(path, s);
        if (!ok) ok = decode_wav_file(path, s);
    }

    if (!ok) {
        free(s);
        return NULL;
    }

    l_info("[audio] loaded %s (%d Hz, %d ch, %u frames)", path, s->rate, s->channels, s->frames);
    return s;
}

static void sfx_enqueue_load(int sndId) {
    pthread_mutex_lock(&gQueueLock);
    bool already_queued = false;
    for (int i = 0; i < gLoadQueueCount; i++) {
        if (gLoadQueue[(gLoadQueueHead + i) % SFX_QUEUE_CAP] == sndId) {
            already_queued = true;
            break;
        }
    }
    if (!already_queued && gLoadQueueCount < SFX_QUEUE_CAP) {
        gLoadQueue[gLoadQueueTail] = sndId;
        gLoadQueueTail = (gLoadQueueTail + 1) % SFX_QUEUE_CAP;
        gLoadQueueCount++;
    } else if (!already_queued) {
        l_warn("[audio] load queue full, dropping request for sndId %d", sndId);
    }
    pthread_mutex_unlock(&gQueueLock);
}

/*
 * `GLMediaPlayer_loadSound/loadSoundBig` are synchronous JNI calls FROM the
 * game engine's own thread, and `minimp3`/`stb_vorbis` (unlike Android's
 * real MediaPlayer.prepare(), which streams instead of decoding up front)
 * fully decode the whole file before returning -- confirmed in
 * logs/asphalt5_020.log taking 4+ seconds for some real raw_*.glsnd files.
 * Decoding here, on the caller's thread, froze the entire game (rendering,
 * input, everything) for that long, not just delayed one sound.
 *
 * Every caller of sfx_get() already tolerates a NULL return gracefully
 * (loadSound/loadSoundBig just ignore it; playSound/playSoundBig silently
 * skip playing that one time) -- exactly the behavior Android's own
 * asynchronous SoundPool.load() already has (a sound requested moments ago
 * simply "isn't ready yet"). So: hand the actual decode off to a background
 * thread and return NULL immediately for anything not already cached,
 * instead of blocking the caller until it's done.
 */
static SfxSample *sfx_get(int sndId) {
    if (sndId < 0 || sndId >= MAX_SOUNDS) return NULL;

    pthread_mutex_lock(&gCacheLock);
    SfxSample *cached = gCache[sndId];
    if (cached == NULL) {
        // First time this sndId has ever been requested -- claim it so a
        // second caller racing in right behind us doesn't also enqueue it.
        gCache[sndId] = SFX_PENDING;
    }
    pthread_mutex_unlock(&gCacheLock);

    if (cached == NULL) {
        sfx_enqueue_load(sndId);
        return NULL;
    }
    if (cached == SFX_FAILED || cached == SFX_PENDING)
        return NULL;
    return cached;
}

/*
 * Polls instead of `pthread_cond_wait`-ing on work: a real crash (data abort
 * inside `pte_osSemaphoreCancellablePend`, reached via `pthread_cond_wait`)
 * hit this exact thread on hardware, TWICE -- once before this project had
 * ever used `pthread_cond_t` at all, and again after adding an explicit
 * `pthread_cond_init()` (the textbook fix for an unusable statically-
 * initialized cond var) made no difference. Rather than keep guessing at
 * `pthread_cond_t` on this platform, this mirrors `video.cpp`'s
 * `cutscene_audio_thread`/`cutscene_audio_submit` -- a mutex-guarded flag
 * plus `sceKernelDelayThread()` polling, no condition variable anywhere --
 * which already runs reliably on hardware elsewhere in this exact codebase.
 * 2ms is well under any latency this queue needs (sound loads are not
 * frame-timing-critical; a sound simply "isn't ready yet" for a couple
 * frames while it decodes, same as before this loop even existed).
 */
static int loader_thread(SceSize args, void *argp) {
    (void) args; (void) argp;
    for (;;) {
        pthread_mutex_lock(&gQueueLock);
        int has_work = gLoadQueueCount > 0;
        int quit = gLoaderQuit;
        int sndId = -1;
        if (has_work) {
            sndId = gLoadQueue[gLoadQueueHead];
            gLoadQueueHead = (gLoadQueueHead + 1) % SFX_QUEUE_CAP;
            gLoadQueueCount--;
        }
        pthread_mutex_unlock(&gQueueLock);

        if (!has_work) {
            if (quit)
                break;
            sceKernelDelayThread(2000);
            continue;
        }

        char path[512];
        SfxSample *s = NULL;
        if (resolve_sound_path(sndId, path, sizeof(path))) {
            s = sfx_load_sample(path);
        }
        if (!s)
            l_warn("[audio] background load failed for sndId %d", sndId);

        pthread_mutex_lock(&gCacheLock);
        gCache[sndId] = s ? s : SFX_FAILED;
        pthread_mutex_unlock(&gCacheLock);
    }
    return 0;
}

static void mix_voice(Voice *v, int *accL, int *accR, int frames) {
    if (!v->smp || v->paused) return;
    SfxSample *s = v->smp;
    for (int i = 0; i < frames; i++) {
        if (v->fadeFramesLeft > 0) {
            v->gain += v->gainStep;
            v->fadeFramesLeft--;
            if (v->fadeFramesLeft == 0) v->gain = v->targetGain;
        }

        unsigned f = (unsigned) v->pos;
        if (f >= s->frames) {
            if (v->loop && s->frames > 0) {
                v->pos = fmod(v->pos, (double) s->frames);
                f = (unsigned) v->pos;
            } else {
                v->smp = NULL;
                break;
            }
        }

        short sl, sr;
        if (s->channels == 1) {
            sl = sr = s->pcm[f];
        } else {
            sl = s->pcm[f * 2];
            sr = s->pcm[f * 2 + 1];
        }

        accL[i] += (int)(sl * v->gain);
        accR[i] += (int)(sr * v->gain);
        v->pos += v->step;
    }
}

static int mixer_thread(SceSize args, void *argp) {
    (void) args; (void) argp;
    static int accL[MIX_GRAIN];
    static int accR[MIX_GRAIN];
    static short outBuf[MIX_GRAIN * 2];

    while (!gQuit) {
        memset(accL, 0, sizeof(accL));
        memset(accR, 0, sizeof(accR));

        pthread_mutex_lock(&gLock);
        mix_voice(&gBig, accL, accR, MIX_GRAIN);
        for (int v = 0; v < MAX_VOICES; v++) {
            mix_voice(&gVoices[v], accL, accR, MIX_GRAIN);
        }
        pthread_mutex_unlock(&gLock);

        for (int i = 0; i < MIX_GRAIN; i++) {
            int l = accL[i];
            int r = accR[i];
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            outBuf[i * 2] = (short) l;
            outBuf[i * 2 + 1] = (short) r;
        }

        sceAudioOutOutput(gPort, outBuf);
    }

    return 0;
}

void audio_init(void) {
    if (gAudioReady) return;

    memset(gCache, 0, sizeof(gCache));
    memset(gVoices, 0, sizeof(gVoices));
    memset(&gBig, 0, sizeof(gBig));

    /*
     * Explicit init rather than trusting the static
     * `= PTHREAD_MUTEX_INITIALIZER` alone -- gCacheLock/gQueueLock are new
     * primitives added alongside the (since removed) gQueueCond condition
     * variable, which crashed on hardware inside `pthread_cond_wait()`
     * twice, including once after adding `pthread_cond_init()` for it made
     * no difference (see loader_thread()'s comment for why the cond var
     * was dropped entirely in favor of polling). Keeping this explicit
     * init for the two mutexes anyway costs nothing and removes any doubt.
     */
    pthread_mutex_init(&gCacheLock, NULL);
    pthread_mutex_init(&gQueueLock, NULL);

    gPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, MIX_GRAIN, MIX_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (gPort < 0) {
        l_error("[audio] sceAudioOutOpenPort failed (0x%08X)", (unsigned) gPort);
        return;
    }

    gQuit = 0;
    // Pin the audio mixer to Core 1 (mask 0x20000) to avoid stealing cycles from the main render thread.
    // Use priority 0x40 (highest normal priority) to prevent audio stuttering (buffer underruns).
    gThread = sceKernelCreateThread("audio_mixer", mixer_thread, 0x40, 0x10000, 0, 0x20000, NULL);
    if (gThread < 0) {
        l_error("[audio] mixer thread creation failed (0x%08X)", (unsigned) gThread);
        sceAudioOutReleasePort(gPort);
        gPort = -1;
        return;
    }
    sceKernelStartThread(gThread, 0, NULL);

    gLoadQueueHead = gLoadQueueTail = gLoadQueueCount = 0;
    gLoaderQuit = 0;
    // Pin the heavy Vorbis loader thread to Core 2 (mask 0x40000) with lowest priority (0x7F)
    // so its software decoding doesn't stutter the game even slightly.
    gLoaderThread = sceKernelCreateThread("audio_loader", loader_thread, 0x7F, 0x10000, 0, 0x40000, NULL);
    if (gLoaderThread < 0) {
        // Not fatal -- playback still works for anything already cached (nothing,
        // this early), it just means sfx_get() will queue requests that never
        // drain. Log loudly since that silently disables all sound.
        l_error("[audio] loader thread creation failed (0x%08X) -- no sound will ever load", (unsigned) gLoaderThread);
    } else {
        sceKernelStartThread(gLoaderThread, 0, NULL);
    }

    gAudioReady = true;
    l_success("[audio] audio subsystem initialized successfully (sceAudioOut 44.1kHz stereo)");
}

void audio_shutdown(void) {
    if (!gAudioReady) return;
    gAudioReady = false;

    if (gLoaderThread >= 0) {
        // loader_thread() polls gLoaderQuit every 2ms (see its own comment
        // for why there's no condition variable to signal here) -- same
        // lock-free flag-then-join style already used for gQuit/gThread
        // right below, nothing new.
        gLoaderQuit = 1;
        sceKernelWaitThreadEnd(gLoaderThread, NULL, NULL);
        sceKernelDeleteThread(gLoaderThread);
        gLoaderThread = -1;
    }

    // Safe now: the loader thread is joined above, so nothing is still
    // waiting on/locking these.
    pthread_mutex_destroy(&gQueueLock);
    pthread_mutex_destroy(&gCacheLock);

    gQuit = 1;
    sceKernelWaitThreadEnd(gThread, NULL, NULL);
    sceKernelDeleteThread(gThread);
    gThread = -1;
    sceAudioOutReleasePort(gPort);
    gPort = -1;

    // Loader thread is joined above, so nothing can still be writing
    // SFX_PENDING into gCache[] at this point -- every entry is NULL,
    // SFX_FAILED, or a real pointer.
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (gCache[i] && gCache[i] != SFX_FAILED) {
            free(gCache[i]->pcm);
            free(gCache[i]);
        }
    }
    memset(gCache, 0, sizeof(gCache));
}

// -------------------- JNI BRIDGE --------------------

// A sound is only "loaded" once the background loader thread has produced a
// real SfxSample* -- SFX_PENDING (queued, still decoding) must read as "not
// loaded yet", not as loaded, or callers that poll this before playing would
// wrongly think a still-decoding sound is ready.
static bool sfx_is_ready(int sndId) {
    if (sndId < 0 || sndId >= MAX_SOUNDS) return false;
    SfxSample *s = gCache[sndId];
    return s != NULL && s != SFX_FAILED && s != SFX_PENDING;
}

jint GLMediaPlayer_isSoundLoaded(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    //return sfx_is_ready(sndId) ? 0 : -1;
    return 0; // Temp fix: always return 0 so the game doesn't hang!
}

jint GLMediaPlayer_isSoundLoadedBig(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    //return sfx_is_ready(sndId) ? 0 : -1;
    return 0; // Temp fix: always return 0 so the game doesn't hang!
}

void GLMediaPlayer_loadSound(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    (void) va_arg(args, jint); // soundInstance
    if (!gAudioReady) return;
    sfx_get(sndId);
}

void GLMediaPlayer_loadSoundBig(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    if (!gAudioReady) return;
    sfx_get(sndId);
}

void audio_play_sound(int sndId, int instance, float vol) {
    if (!gAudioReady) return;

    SfxSample *s = sfx_get(sndId);
    if (!s) return;

    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;

    pthread_mutex_lock(&gLock);
    Voice *v = NULL;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!gVoices[i].smp) { v = &gVoices[i]; break; }
    }
    if (!v) v = &gVoices[0]; // steal oldest

    v->pos = 0.0;
    v->step = (double) s->rate / (double) MIX_RATE;
    v->gain = v->targetGain = vol;
    v->fadeFramesLeft = 0;
    v->gainStep = 0.0f;
    v->loop = false;
    v->paused = false;
    v->sndId = sndId;
    v->instance = instance;
    v->smp = s;
    pthread_mutex_unlock(&gLock);
}

void audio_play_sound_big(int sndId, float vol, int loop) {
    if (!gAudioReady) return;

    SfxSample *s = sfx_get(sndId);
    if (!s) return;

    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;

    pthread_mutex_lock(&gLock);
    gBig.pos = 0.0;
    gBig.step = (double) s->rate / (double) MIX_RATE;
    gBig.gain = gBig.targetGain = vol;
    gBig.fadeFramesLeft = 0;
    gBig.gainStep = 0.0f;
    gBig.loop = loop ? true : false;
    gBig.paused = false;
    gBig.sndId = sndId;
    gBig.instance = -1;
    gBig.smp = s;
    pthread_mutex_unlock(&gLock);
    l_info("[audio] playSoundBig: sndId=%d (vol=%.2f, loop=%d)", sndId, (double) vol, loop);
}

void audio_stop_all(void) {
    if (!gAudioReady) return;
    pthread_mutex_lock(&gLock);
    for (int i = 0; i < MAX_VOICES; i++) gVoices[i].smp = NULL;
    gBig.smp = NULL;
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_playSound(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    int instance = va_arg(args, jint);
    float vol = (float) va_arg(args, double);
    audio_play_sound(sndId, instance, vol);
}

void GLMediaPlayer_playSoundBig(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    float vol = (float) va_arg(args, double);
    audio_play_sound_big(sndId, vol, 1);
}

void GLMediaPlayer_setLoopBig(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    jboolean loop = va_arg(args, jint);
    pthread_mutex_lock(&gLock);
    if (gBig.smp && gBig.sndId == sndId) {
        gBig.loop = loop ? true : false;
    }
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_pauseSound(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    int instance = va_arg(args, jint);
    pthread_mutex_lock(&gLock);
    for (int i = 0; i < MAX_VOICES; i++) {
        if (gVoices[i].smp && gVoices[i].sndId == sndId && gVoices[i].instance == instance)
            gVoices[i].paused = true;
    }
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_pauseSoundBig(jmethodID id, va_list args) {
    (void) id;
    (void) args;
    pthread_mutex_lock(&gLock);
    gBig.paused = true;
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_resumeSound(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    int instance = va_arg(args, jint);
    pthread_mutex_lock(&gLock);
    for (int i = 0; i < MAX_VOICES; i++) {
        if (gVoices[i].smp && gVoices[i].sndId == sndId && gVoices[i].instance == instance)
            gVoices[i].paused = false;
    }
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_resumeSoundBig(jmethodID id, va_list args) {
    (void) id;
    (void) args;
    pthread_mutex_lock(&gLock);
    gBig.paused = false;
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_stopSound(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    int instance = va_arg(args, jint);
    pthread_mutex_lock(&gLock);
    for (int i = 0; i < MAX_VOICES; i++) {
        if (gVoices[i].smp && gVoices[i].sndId == sndId && gVoices[i].instance == instance)
            gVoices[i].smp = NULL;
    }
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_stopSoundBig(jmethodID id, va_list args) {
    (void) id;
    (void) args;
    pthread_mutex_lock(&gLock);
    gBig.smp = NULL;
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_unloadSound(jmethodID id, va_list args) { (void) id; (void) args; }
void GLMediaPlayer_unloadSoundBig(jmethodID id, va_list args) { (void) id; (void) args; }

void GLMediaPlayer_setVolume(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    int instance = va_arg(args, jint);
    float vol = (float) va_arg(args, double);
    pthread_mutex_lock(&gLock);
    for (int i = 0; i < MAX_VOICES; i++) {
        if (gVoices[i].smp && gVoices[i].sndId == sndId && gVoices[i].instance == instance)
            gVoices[i].gain = gVoices[i].targetGain = vol;
    }
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_setVolumeBig(jmethodID id, va_list args) {
    (void) id;
    (void) va_arg(args, jint);
    float vol = (float) va_arg(args, double);
    pthread_mutex_lock(&gLock);
    gBig.gain = gBig.targetGain = vol;
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_resetSound(jmethodID id, va_list args) { (void) id; (void) args; }
void GLMediaPlayer_setPitch(jmethodID id, va_list args) { (void) id; (void) args; }

void GLMediaPlayer_stopAllSounds(jmethodID id, va_list args) {
    (void) id; (void) args;
    pthread_mutex_lock(&gLock);
    for (int i = 0; i < MAX_VOICES; i++) gVoices[i].smp = NULL;
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_stopAllPool(jmethodID id, va_list args) {
    GLMediaPlayer_stopAllSounds(id, args);
}

void GLMediaPlayer_stopAllBig(jmethodID id, va_list args) {
    (void) id; (void) args;
    pthread_mutex_lock(&gLock);
    gBig.smp = NULL;
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_destroySoundPool(jmethodID id, va_list args) { (void) id; (void) args; }
void GLMediaPlayer_initSoundPoolArray(jmethodID id, va_list args) { (void) id; (void) args; }

jboolean GLMediaPlayer_isMediaPlaying(jmethodID id, va_list args) {
    (void) id; (void) args;
    return gBig.smp != NULL && !gBig.paused;
}
