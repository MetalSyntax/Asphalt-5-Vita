/**
 * @file video.cpp
 * @brief Cutscene video playback via software MPEG-4/AAC decode (FFmpeg),
 *        threaded, presented at a fixed 30 fps like the two reference
 *        ports (Prince of Persia, Dungeon-Hunter-2-vita).
 *
 * @details The PS Vita's hardware video decoder (`SceVideodec`, used
 * internally by `SceAvPlayer`) only decodes H.264/AVC -- it has no hardware
 * path for MPEG-4 Part 2, which is what every `.mp4` asset this game ships
 * actually is (`mp4v` fourcc, confirmed via `ffprobe` on all 7: the trailer
 * is 800x480 Simple Profile @ 30000/1001, the five in-race videos are
 * 848x480 Advanced Simple Profile @ 24 -- the `_H264` suffix some of the
 * filenames have does NOT reflect their real codec). Confirmed on hardware:
 * `SceAvPlayer` opens/demuxes one of these files fine but the decoder never
 * produces a single frame and self-stops after ~1.6s with no error event at
 * all -- see port_progress.md's "Bug #17". Re-encoding the game's original
 * assets to H.264 was tried and explicitly rejected: a port that requires
 * modifying the original game's data files to work isn't a real port.
 *
 * So this file decodes the ORIGINAL MPEG-4 Part 2 / AAC files as shipped,
 * entirely in software, via FFmpeg's `libavformat`/`libavcodec`/
 * `libswresample` (needs `vdpm ffmpeg` + `vdpm lame` once, see the comment
 * next to the ffmpeg `target_link_libraries` entries in CMakeLists.txt).
 *
 * ---------------------------------------------------------------------------
 * Why the FFmpeg-based version never played anything (CONFIRMED, not a guess)
 * ---------------------------------------------------------------------------
 * Every log from that version ends the same way, `ux0:`-URL parsing already
 * fixed (a custom `AVIOContext` reads via `sceIoOpen`/`sceIoRead`/
 * `sceIoLseek`, so FFmpeg never sees the path string at all):
 *
 *     [INFO   ] video: No explicit demuxer found, letting FFmpeg auto-detect
 *     [INFO   ] video: file read #1 len=65536 -> 65536
 *     ... (reads double: 64K, 64K, 128K, 256K, 512K -- exactly
 *          av_probe_input_buffer2()'s generic-probe growth, up to its 1MB cap)
 *     [ERROR  ] video: avformat_open_input failed (-1094995529: Invalid data
 *               found when processing input) for .../A5_Ultimate_VNFS_2_854.mp4
 *
 * The bytes read are genuine, valid MP4 (`ffprobe` on a host machine parses
 * the exact same file fine). What is missing is the demuxer:
 * `av_find_input_format("mov")` returns NULL, so `avformat_open_input()`
 * always falls through to that generic auto-probe -- and it never succeeds
 * either, because **no demuxer for MP4/MOV exists anywhere in this FFmpeg
 * build to claim it.** Confirmed directly against the installed static lib:
 *
 *     $ ar t ~/vitasdk/arm-vita-eabi/lib/libavformat.a | grep -i mov
 *     mov_chan.o/ mov_esds.o/ movenc.o/ movenc_ttml.o/ movenccenc.o/ movenchint.o/
 *
 * That is the MP4 *muxer* (`movenc.o`) plus two small shared helpers used by
 * other formats -- `mov.o`, the file containing `ff_mov_demuxer` itself,
 * simply is not in the archive, even though the vita-portlibs `ffmpeg`
 * recipe's embedded configure string does list `--enable-demuxer=...,mp4,
 * m4a,...`. That is a vita-portlibs build bug, upstream of this repo, and
 * not something fixable from `source/`.
 *
 * Fixed here by not needing that demuxer at all: `mp4_open()`/
 * `mp4_next_packet()` below parse the small slice of the ISO-BMFF box tree
 * (`moov`/`trak`/`mdia`/`minf`/`stbl`) needed to hand libavcodec's MPEG-4
 * Part 2 and AAC decoders -- both very much present and working in this same
 * build -- their two elementary streams directly, packet by packet, reading
 * via plain `sceIoOpen`/`sceIoRead`/`sceIoLseek` (no `AVIOContext`, no
 * `AVFormatContext` -- `libavformat` is not used by this file at all
 * anymore). See the "Minimal MP4 (ISO-BMFF) demuxer" section below for the
 * parser itself.
 *
 * ---------------------------------------------------------------------------
 * Architecture (and what it borrows from the two reference ports)
 * ---------------------------------------------------------------------------
 * Both reference ports get their frame rate for free: `SceAvPlayer` demuxes,
 * decodes and A/V-paces on its own internal threads, and their playback loop
 * just polls `sceAvPlayerGetVideoData()`/`GetAudioData()` and sleeps 1ms.
 * Nothing decodes on their render thread. This port cannot use SceAvPlayer,
 * so that same shape is rebuilt here in software -- which is the whole reason
 * the video can now hold a fixed 30 fps instead of whatever a single core
 * happens to manage:
 *
 *   - **Decode thread** (`video_decode_thread`, cores 1-2): demuxes, decodes
 *     video, converts YUV420P -> RGB565 with NEON, and publishes finished
 *     frames into a 3-slot ring with their presentation timestamps. Audio
 *     packets are handed off to the audio thread still compressed.
 *   - **Audio thread** (`cutscene_audio_thread`, core 2): decodes AAC,
 *     resamples to interleaved S16 and feeds `sceAudioOut` in fixed
 *     1024-frame blocks. `sceAudioOutOutput()` blocks until the hardware is
 *     ready, so this thread paces itself against the real audio clock.
 *   - **Render thread** (the caller: `GS_TrailerMovie::Create()` ->
 *     `GLMediaPlayer.loadMovie()` -> here, i.e. the thread that owns the GXM
 *     context): only ever uploads the frame that is due, draws it and swaps.
 *     No decoding, no color conversion. GL stays single-threaded, which
 *     vitaGL requires.
 *
 * Video is paced against the **audio hardware clock** (samples actually
 * played back, not wall time), so the two can never drift apart: if decode
 * ever falls behind, audio playback -- and therefore the clock -- slows with
 * it, and A/V stays in sync instead of desynchronising progressively. With
 * no usable audio stream it falls back to a plain wall clock, and a watchdog
 * also falls back if the audio clock ever stalls for 2s.
 *
 * Nothing uses `pthread_cond_t`: every wait in this file is a mutex-guarded
 * flag plus `sceKernelDelayThread()` polling. `pthread_cond_wait()` is
 * confirmed-broken for threads in this port (a real data abort inside
 * `pte_osSemaphoreCancellablePend`, twice, including after an explicit
 * `pthread_cond_init()`) -- see the comment on `loader_thread()` in
 * audio.cpp. The corollary, and the reason both workers here are created
 * with `pthread_create()` rather than `sceKernelCreateThread()` like the
 * rest of this port's threads: FFmpeg's own frame-threading waits on
 * condition variables internally, on whichever thread calls
 * `avcodec_receive_frame()`. That is only legal from a thread libpthread
 * knows about, so the decode thread has to be a real pthread (its priority
 * and core affinity are then set from inside the thread itself, which gets
 * the same placement `sceKernelCreateThread()` would have given it).
 *
 * Reused from the previous version: the NEON YUV420P->RGB565 conversion
 * (slimmed to Q7 int16 chroma math in the Bug #25 fps fix -- same output
 * within 1 RGB565 LSB, see the comment on the converter itself), and `draw_video_frame()`'s plain GLES1.1 fixed-function
 * texture-quad rendering (do NOT reintroduce a custom GLSL shader here --
 * that caused a confirmed-on-hardware regression, see the comment on
 * `draw_video_frame()` below, and it is the one thing deliberately NOT taken
 * from Dungeon-Hunter-2-vita, whose video path does convert YUV on the GPU).
 */

#include "video.h"
#include "utils/logger.h"
#include "utils/glutil.h"

#include <psp2/ctrl.h>
#include <psp2/audioout.h>
#include <psp2/power.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

#include <arm_neon.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

/*
 * Ring of finished (already color-converted) frames between the decode
 * thread and the render thread. 3 slots is a frame being displayed, a frame
 * ready to go, and a frame being converted -- enough to absorb FFmpeg's
 * frame-threading latency and the jitter of individual frames without
 * letting decode run so far ahead that it wastes RAM (each slot is
 * w*h*2 bytes: 750KB at 800x480, 795KB at 848x480). Slots are allocated on
 * demand and freed when playback ends, so nothing is held between videos.
 */
#define VIDEO_FRAME_SLOTS 3

/*
 * Compressed-audio queue depth, in packets. One AAC packet is 1024 samples
 * (~23ms at 44.1kHz), so this is ~1.5s of slack. It exists to decouple the
 * two workers: the decode thread stalls for up to a frame period at a time
 * whenever the frame ring is full (that stall is exactly what paces decode
 * to real time), and without a buffer here that stall would starve audio
 * output and click. It is deep enough that the audio thread never runs dry
 * across such a stall, and bounded so a coarsely-interleaved .mp4 can't
 * queue the whole audio track in RAM.
 */
#define AUDIO_PKT_QUEUE_CAP 64

/*
 * Frames per channel handed to sceAudioOutOutput() per call, fixed at open
 * time. A Vita audio port only ever accepts exactly this many frames per
 * call, which is why resampled output is accumulated and emitted in whole
 * blocks instead of being submitted one decoded AAC frame at a time the way
 * the previous version did (that happened to work only because AAC frames
 * are 1024 samples -- it would have submitted a wrong-length buffer the
 * moment swr_convert() returned any other count). Must be a multiple of 64.
 */
#define AUDIO_GRAIN 1024

/*
 * Fixed presentation period: 30 fps, like both reference ports (Prince of
 * Persia / Dungeon-Hunter-2-vita `source/video.cpp`), whose SceAvPlayer
 * loops just draw each decoded frame with a 1ms poll and land on ~30 fps.
 * The trailer is 29.97 fps and the in-race videos 24 fps, but presentation
 * here runs on a fixed 33.33ms tick so every video plays at the same rate:
 * per-frame pts still order the frames, the tick sets the cadence (see the
 * present loop in video_play()). Also used as the spacing for frames that
 * arrive with no timestamp at all.
 */
#define VIDEO_FRAME_PERIOD_US 33333

/*
 * Frame-level threading for the video decoder -- OFF (Bug #25, CONFIRMED on
 * hardware, see port_progress.md log #067). This used to be 3, on the theory
 * that "use every core" would let libavcodec's frame threads overlap with the
 * render thread's mostly-idle 33ms. In practice it made playback three times
 * *slower* than the target: log #067 measured yuv_convert (the NEON
 * conversion below, timed tightly around just that one call) at
 * 98.1ms/frame average -- avg_fps=10.0
 * -- on a CPU confirmed running at 444MHz (see scePowerSetArmClockFrequency
 * in utils/init.c), where that conversion is a few ms of integer NEON work
 * at most. The gap is pure scheduling contention, not compute: libavcodec's
 * own frame-thread pool is created with plain pthread_create() (no Vita
 * affinity call reaches it, unlike every thread this file itself spawns), so
 * those threads default to roaming cores 0-2 freely -- stacking up to 3 more
 * runnable threads onto the same two physical cores this file's own decode
 * thread (cores 1-2, see video_decode_thread()) and the audio thread (core 2,
 * real-time priority) already share, and even spilling onto the render
 * thread's core 0. The result is the *outer* thread -- the one actually
 * timed here -- losing its slice again and again mid-conversion, which shows
 * up entirely as inflated wall-clock cost on this one line. Dropping to a
 * single decode thread removes libavcodec's pool outright: now only this
 * file's own (Vita-affinitized) threads compete for cores 1-2, at a 1:1-ish
 * ratio the scheduler can actually keep up with. Bonus: it also closes a
 * latent correctness gap -- handle_decoded_frame() mutates
 * `P.vctx->skip_frame` between calls to catch up when playback falls behind,
 * which is only well-defined if frames are decoded one at a time in that
 * same order; under frame-threading it could apply to whichever frame(s)
 * libavcodec's internal threads happened to have in flight.
 */
#define VIDEO_DECODE_THREADS 1

static const AVRational kUsTimeBase = { 1, 1000000 };

static inline int64_t now_us(void) {
    return (int64_t) sceKernelGetProcessTimeWide();
}

// ---------------------------------------------------------------------------
// Minimal MP4 (ISO-BMFF) demuxer
// ---------------------------------------------------------------------------
//
// Stands in for libavformat's "mov" demuxer, which this vita-portlibs
// FFmpeg build does not actually contain -- see the file header for how
// that was confirmed (`ar t libavformat.a` has no `mov.o`, only the muxer
// side). Parses just enough of the box tree (moov/trak/mdia/minf/stbl) to
// hand libavcodec's MPEG-4 Part 2 / AAC decoders their two elementary
// streams directly, packet by packet, in the same interleaved order they
// already sit in the file -- no seeking backwards, no re-muxing, the
// original .mp4 assets read completely unmodified. Everything downstream
// (avcodec_send_packet()/avcodec_receive_frame(), the frame ring, the audio
// resampler/output) is unchanged.
//
// Scoped deliberately to what this game's 7 fixed .mp4 assets actually use
// (confirmed via ffprobe on all 7): one video + one audio track, one sample
// description each, no edit lists, no fragmentation (moof/mfra). This is
// not a general-purpose MP4 parser.

struct Mp4Sample {
    uint64_t offset;     // absolute file offset
    uint32_t size;
    int64_t  dts;        // decode timestamp, in the track's own timescale
    int32_t  cts_delta;  // composition offset (ctts), same timescale; 0 if none
};

struct Mp4Track {
    bool       present;
    uint32_t   timescale;
    AVCodecID  codec_id;
    int        width, height;          // video only
    int        channels, sample_rate;  // audio only
    uint8_t   *extradata;
    int        extradata_size;
    Mp4Sample *samples;
    uint32_t   sample_count;
    uint32_t   next;                   // read cursor, playback only
};

#define MP4_STREAM_VIDEO 0
#define MP4_STREAM_AUDIO 1

struct Mp4File {
    SceUID   fd;
    Mp4Track track[2];  // indexed by MP4_STREAM_VIDEO / MP4_STREAM_AUDIO
};

static inline uint32_t mp4_be32(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}
static inline uint64_t mp4_be64(const uint8_t *p) {
    return ((uint64_t) mp4_be32(p) << 32) | mp4_be32(p + 4);
}
#define MP4_FOURCC(a, b, c, d) \
    (((uint32_t) (a) << 24) | ((uint32_t) (b) << 16) | ((uint32_t) (c) << 8) | (uint32_t) (d))

struct Mp4Box {
    uint32_t type;
    uint64_t body_off, body_end;
};

static bool mp4_read_box(SceUID fd, uint64_t pos, uint64_t limit, Mp4Box *b) {
    if (pos + 8 > limit)
        return false;
    uint8_t hdr[8];
    if (sceIoLseek(fd, (SceOff) pos, SCE_SEEK_SET) < 0 || sceIoRead(fd, hdr, 8) != 8)
        return false;
    uint64_t size = mp4_be32(hdr);
    uint32_t type = mp4_be32(hdr + 4);
    uint64_t hdr_size = 8;
    if (size == 1) {
        uint8_t ext[8];
        if (sceIoRead(fd, ext, 8) != 8)
            return false;
        size = mp4_be64(ext);
        hdr_size = 16;
    } else if (size == 0) {
        size = limit - pos;
    }
    if (size < hdr_size || pos + size > limit)
        return false;
    b->type = type;
    b->body_off = pos + hdr_size;
    b->body_end = pos + size;
    return true;
}

// Every box body this parser reads in full (stsd/stts/ctts/stsc/stsz/stco)
// is at most a few tens of KB even for this game's longest (54s) video, so
// there is no need to stream them -- read the whole thing and parse in
// memory. Caller av_frees the result.
static uint8_t *mp4_read_range(SceUID fd, uint64_t off, uint64_t end, uint32_t *out_len) {
    uint64_t len = end - off;
    if (len == 0 || len > (16u << 20))  // sanity cap; real boxes here are tiny
        return NULL;
    uint8_t *buf = (uint8_t *) av_malloc((size_t) len);
    if (!buf)
        return NULL;
    if (sceIoLseek(fd, (SceOff) off, SCE_SEEK_SET) < 0 ||
        sceIoRead(fd, buf, (SceSize) len) != (int) len) {
        av_free(buf);
        return NULL;
    }
    *out_len = (uint32_t) len;
    return buf;
}

/*
 * Walks an MPEG-4 descriptor tree (ISO/IEC 14496-1) inside an `esds` box
 * looking for tag 0x05, DecoderSpecificInfo -- the raw codec config (an
 * MPEG-4 VOL header for video, AudioSpecificConfig for AAC) libavcodec
 * needs as `extradata`. Recurses into ES_Descr (0x03) and
 * DecoderConfigDescr (0x04), the only two tags that can contain it here,
 * skipping their fixed-size fields first (including the couple of ES_Descr
 * fields only present when specific flag bits are set).
 */
static bool mp4_find_dec_specific_info(const uint8_t *buf, uint32_t len,
                                        const uint8_t **out, uint32_t *out_len) {
    uint32_t pos = 0;
    while (pos + 2 <= len) {
        uint8_t tag = buf[pos++];
        uint32_t dlen = 0;
        bool cont = true;
        for (int i = 0; i < 4 && cont; i++) {
            if (pos >= len) return false;
            uint8_t b = buf[pos++];
            dlen = (dlen << 7) | (b & 0x7f);
            cont = (b & 0x80) != 0;
        }
        if (pos + dlen > len)
            return false;
        if (tag == 0x05) {
            *out = buf + pos;
            *out_len = dlen;
            return true;
        }
        if (tag == 0x03 || tag == 0x04) {
            uint32_t skip;
            if (tag == 0x03) {
                if (dlen < 3) return false;
                uint8_t flags = buf[pos + 2];
                skip = 3;
                if (flags & 0x80) skip += 2;
                if (flags & 0x40) { if (skip >= dlen) return false; skip += 1 + buf[pos + skip]; }
                if (flags & 0x20) skip += 2;
            } else {
                skip = 1 + 1 + 3 + 4 + 4;  // objectType, streamType/flags, bufferSizeDB, max/avgBitrate
            }
            if (skip <= dlen && mp4_find_dec_specific_info(buf + pos + skip, dlen - skip, out, out_len))
                return true;
        }
        pos += dlen;
    }
    return false;
}

// stsd: reads the track's first (only, for these assets) sample entry --
// codec fourcc, width/height or channels/rate -- and pulls
// DecoderSpecificInfo out of its nested `esds` box for extradata.
static void mp4_parse_stsd(SceUID fd, uint64_t off, uint64_t end, Mp4Track *trk, bool is_audio) {
    uint32_t len;
    uint8_t *body = mp4_read_range(fd, off, end, &len);
    if (!body) return;
    if (len < 16) { av_free(body); return; }

    uint32_t entry_off = 8;  // past version/flags(4)+entry_count(4); first SampleEntry starts here
    uint32_t entry_size = mp4_be32(body + entry_off);
    uint32_t format = mp4_be32(body + entry_off + 4);
    // SampleEntry box header(8) + reserved[6]+data_reference_index(2) + the
    // Audio/VisualSampleEntry's own fixed fields (20 / 70 bytes).
    uint32_t fixed_size = is_audio ? (8 + 8 + 20) : (8 + 8 + 70);
    if (entry_off + fixed_size > len) { av_free(body); return; }

    if (is_audio) {
        trk->codec_id = (format == MP4_FOURCC('m', 'p', '4', 'a')) ? AV_CODEC_ID_AAC : AV_CODEC_ID_NONE;
        trk->channels = (int) (mp4_be32(body + entry_off + 24) >> 16);
        trk->sample_rate = (int) (mp4_be32(body + entry_off + 32) >> 16);
    } else {
        trk->codec_id = (format == MP4_FOURCC('m', 'p', '4', 'v')) ? AV_CODEC_ID_MPEG4 : AV_CODEC_ID_NONE;
        uint32_t wh = mp4_be32(body + entry_off + 32);
        trk->width = (int) (wh >> 16);
        trk->height = (int) (wh & 0xFFFF);
    }

    uint32_t child_off = entry_off + fixed_size;
    uint32_t child_end = (entry_size >= fixed_size && entry_off + entry_size <= len) ? entry_off + entry_size : len;
    while (child_off + 8 <= child_end) {
        uint32_t bsize = mp4_be32(body + child_off);
        uint32_t btype = mp4_be32(body + child_off + 4);
        if (bsize < 8 || child_off + bsize > child_end) break;
        if (btype == MP4_FOURCC('e', 's', 'd', 's') && bsize > 12) {
            const uint8_t *desc = body + child_off + 12;  // box header(8) + FullBox version/flags(4)
            uint32_t desc_len = bsize - 12;
            const uint8_t *info; uint32_t info_len;
            if (mp4_find_dec_specific_info(desc, desc_len, &info, &info_len) && info_len > 0) {
                trk->extradata = (uint8_t *) av_mallocz(info_len + AV_INPUT_BUFFER_PADDING_SIZE);
                if (trk->extradata) {
                    memcpy(trk->extradata, info, info_len);
                    trk->extradata_size = (int) info_len;
                }
            }
            break;
        }
        child_off += bsize;
    }
    av_free(body);
}

// stbl: locates stsd/stts/ctts/stsc/stsz/stco (or co64), then builds the
// track's flat, chunk-ordered sample list (offset/size/dts/cts per sample)
// -- the same table every MP4 demuxer builds, just done once up front here
// instead of lazily.
static bool mp4_parse_stbl(SceUID fd, uint64_t off, uint64_t end, Mp4Track *trk, bool is_audio) {
    uint64_t stsd_off = 0, stsd_end = 0, stts_off = 0, stts_end = 0;
    uint64_t ctts_off = 0, ctts_end = 0, stsc_off = 0, stsc_end = 0;
    uint64_t stsz_off = 0, stsz_end = 0, stco_off = 0, stco_end = 0;
    bool have_stsd = false, have_stts = false, have_ctts = false;
    bool have_stsc = false, have_stsz = false, have_stco = false, stco_is64 = false;

    for (uint64_t pos = off; pos < end; ) {
        Mp4Box b;
        if (!mp4_read_box(fd, pos, end, &b)) break;
        if (b.type == MP4_FOURCC('s', 't', 's', 'd')) { stsd_off = b.body_off; stsd_end = b.body_end; have_stsd = true; }
        else if (b.type == MP4_FOURCC('s', 't', 't', 's')) { stts_off = b.body_off; stts_end = b.body_end; have_stts = true; }
        else if (b.type == MP4_FOURCC('c', 't', 't', 's')) { ctts_off = b.body_off; ctts_end = b.body_end; have_ctts = true; }
        else if (b.type == MP4_FOURCC('s', 't', 's', 'c')) { stsc_off = b.body_off; stsc_end = b.body_end; have_stsc = true; }
        else if (b.type == MP4_FOURCC('s', 't', 's', 'z')) { stsz_off = b.body_off; stsz_end = b.body_end; have_stsz = true; }
        else if (b.type == MP4_FOURCC('s', 't', 'c', 'o')) { stco_off = b.body_off; stco_end = b.body_end; have_stco = true; stco_is64 = false; }
        else if (b.type == MP4_FOURCC('c', 'o', '6', '4')) { stco_off = b.body_off; stco_end = b.body_end; have_stco = true; stco_is64 = true; }
        pos = b.body_end;
    }
    if (!have_stsd || !have_stts || !have_stsc || !have_stsz || !have_stco)
        return false;

    mp4_parse_stsd(fd, stsd_off, stsd_end, trk, is_audio);
    if (trk->codec_id == AV_CODEC_ID_NONE)
        return false;

    // ---- stsz: sample count + per-sample size (or one constant size) ----
    uint32_t stsz_len;
    uint8_t *stsz_body = mp4_read_range(fd, stsz_off, stsz_end, &stsz_len);
    if (!stsz_body || stsz_len < 12) { av_free(stsz_body); return false; }
    uint32_t const_size = mp4_be32(stsz_body + 4);
    uint32_t sample_count = mp4_be32(stsz_body + 8);
    if (sample_count == 0 || (const_size == 0 && stsz_len < 12 + (uint64_t) sample_count * 4)) {
        av_free(stsz_body);
        return false;
    }

    Mp4Sample *samples = (Mp4Sample *) av_mallocz(sample_count * sizeof(Mp4Sample));
    if (!samples) { av_free(stsz_body); return false; }
    for (uint32_t i = 0; i < sample_count; i++)
        samples[i].size = const_size ? const_size : mp4_be32(stsz_body + 12 + i * 4);
    av_free(stsz_body);

    // ---- stts: expand (count,delta) runs into a per-sample dts ----
    uint32_t stts_len;
    uint8_t *stts_body = mp4_read_range(fd, stts_off, stts_end, &stts_len);
    if (!stts_body || stts_len < 4) { av_free(stts_body); av_free(samples); return false; }
    uint32_t stts_entries = mp4_be32(stts_body + 4);
    int64_t dts = 0;
    uint32_t si = 0;
    for (uint32_t e = 0; e < stts_entries && si < sample_count; e++) {
        uint64_t eoff = 8 + (uint64_t) e * 8;
        if (eoff + 8 > stts_len) break;
        uint32_t count = mp4_be32(stts_body + eoff);
        uint32_t delta = mp4_be32(stts_body + eoff + 4);
        for (uint32_t k = 0; k < count && si < sample_count; k++, si++) {
            samples[si].dts = dts;
            dts += delta;
        }
    }
    av_free(stts_body);

    // ---- ctts (optional): same run-length shape, signed offsets ----
    if (have_ctts) {
        uint32_t ctts_len;
        uint8_t *ctts_body = mp4_read_range(fd, ctts_off, ctts_end, &ctts_len);
        if (ctts_body && ctts_len >= 4) {
            uint32_t ctts_entries = mp4_be32(ctts_body + 4);
            si = 0;
            for (uint32_t e = 0; e < ctts_entries && si < sample_count; e++) {
                uint64_t eoff = 8 + (uint64_t) e * 8;
                if (eoff + 8 > ctts_len) break;
                uint32_t count = mp4_be32(ctts_body + eoff);
                int32_t coff = (int32_t) mp4_be32(ctts_body + eoff + 4);
                for (uint32_t k = 0; k < count && si < sample_count; k++, si++)
                    samples[si].cts_delta = coff;
            }
        }
        av_free(ctts_body);
    }

    // ---- stsc + stco/co64: chunk layout -> absolute per-sample offset ----
    uint32_t stsc_len = 0;
    uint8_t *stsc_body = mp4_read_range(fd, stsc_off, stsc_end, &stsc_len);
    uint32_t stco_len = 0;
    uint8_t *stco_body = mp4_read_range(fd, stco_off, stco_end, &stco_len);
    bool ok = stsc_body && stco_body && stsc_len >= 4 && stco_len >= 4;
    if (ok) {
        uint32_t stsc_entries = mp4_be32(stsc_body + 4);
        uint32_t stride = stco_is64 ? 8 : 4;
        uint32_t num_chunks = mp4_be32(stco_body + 4);
        ok = stsc_entries > 0 && num_chunks > 0 &&
             stsc_len >= 8 + (uint64_t) stsc_entries * 12 &&
             stco_len >= 8 + (uint64_t) num_chunks * stride;
        if (ok) {
            uint32_t stsc_idx = 0;
            si = 0;
            for (uint32_t chunk = 0; chunk < num_chunks && si < sample_count; chunk++) {
                uint32_t chunk1 = chunk + 1;
                while (stsc_idx + 1 < stsc_entries &&
                       mp4_be32(stsc_body + 8 + (stsc_idx + 1) * 12) <= chunk1)
                    stsc_idx++;
                uint32_t spc = mp4_be32(stsc_body + 8 + stsc_idx * 12 + 4);
                uint64_t coff = stco_is64 ? mp4_be64(stco_body + 8 + (uint64_t) chunk * 8)
                                          : mp4_be32(stco_body + 8 + (uint64_t) chunk * 4);
                for (uint32_t s = 0; s < spc && si < sample_count; s++, si++) {
                    samples[si].offset = coff;
                    coff += samples[si].size;
                }
            }
            ok = (si == sample_count);
        }
    }
    av_free(stsc_body);
    av_free(stco_body);
    if (!ok) { av_free(samples); return false; }

    trk->samples = samples;
    trk->sample_count = sample_count;
    trk->present = true;
    return true;
}

// Recurses moov -> trak -> mdia -> minf -> stbl, and reads mdia's own mdhd
// (timescale) and hdlr (handler_type: 'vide'/'soun') along the way to know
// which of the two tracks this is and in what timescale its stts/ctts
// timestamps are expressed.
static void mp4_parse_trak(SceUID fd, uint64_t off, uint64_t end, Mp4Track *video, Mp4Track *audio) {
    uint32_t timescale = 0;
    bool is_video = false, is_audio = false;
    uint64_t stbl_off = 0, stbl_end = 0;
    bool have_stbl = false;

    for (uint64_t pos = off; pos < end; ) {
        Mp4Box b;
        if (!mp4_read_box(fd, pos, end, &b)) break;
        if (b.type == MP4_FOURCC('m', 'd', 'i', 'a')) {
            for (uint64_t p2 = b.body_off; p2 < b.body_end; ) {
                Mp4Box c;
                if (!mp4_read_box(fd, p2, b.body_end, &c)) break;
                if (c.type == MP4_FOURCC('m', 'd', 'h', 'd')) {
                    uint32_t len;
                    uint8_t *body = mp4_read_range(fd, c.body_off, c.body_end, &len);
                    if (body) {
                        uint8_t version = body[0];
                        timescale = version == 1 ? mp4_be32(body + 20) : mp4_be32(body + 12);
                        av_free(body);
                    }
                } else if (c.type == MP4_FOURCC('h', 'd', 'l', 'r')) {
                    uint32_t len;
                    uint8_t *body = mp4_read_range(fd, c.body_off, c.body_end, &len);
                    if (body && len >= 12) {
                        uint32_t handler = mp4_be32(body + 8);
                        is_video = (handler == MP4_FOURCC('v', 'i', 'd', 'e'));
                        is_audio = (handler == MP4_FOURCC('s', 'o', 'u', 'n'));
                    }
                    av_free(body);
                } else if (c.type == MP4_FOURCC('m', 'i', 'n', 'f')) {
                    for (uint64_t p3 = c.body_off; p3 < c.body_end; ) {
                        Mp4Box d;
                        if (!mp4_read_box(fd, p3, c.body_end, &d)) break;
                        if (d.type == MP4_FOURCC('s', 't', 'b', 'l')) {
                            stbl_off = d.body_off; stbl_end = d.body_end; have_stbl = true;
                        }
                        p3 = d.body_end;
                    }
                }
                p2 = c.body_end;
            }
        }
        pos = b.body_end;
    }

    if (!have_stbl || timescale == 0 || (!is_video && !is_audio))
        return;

    Mp4Track *trk = is_video ? video : audio;
    if (trk->present)
        return;  // one video + one audio track expected; ignore any extra
    trk->timescale = timescale;
    mp4_parse_stbl(fd, stbl_off, stbl_end, trk, is_audio);
    if (!trk->present)
        memset(trk, 0, sizeof(*trk));  // parse failed partway through; leave it absent
}

static bool mp4_open(const char *path, Mp4File *mp4) {
    memset(mp4, 0, sizeof(*mp4));
    mp4->fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (mp4->fd < 0) {
        l_error("video: sceIoOpen failed for %s (0x%08X)", path, (unsigned) mp4->fd);
        return false;
    }
    int64_t size = (int64_t) sceIoLseek(mp4->fd, 0, SCE_SEEK_END);
    l_info("video: opened %s (%lld bytes)", path, (long long) size);

    uint64_t moov_off = 0, moov_end = 0;
    bool have_moov = false;
    for (uint64_t pos = 0; pos < (uint64_t) size; ) {
        Mp4Box b;
        if (!mp4_read_box(mp4->fd, pos, (uint64_t) size, &b)) break;
        if (b.type == MP4_FOURCC('m', 'o', 'o', 'v')) { moov_off = b.body_off; moov_end = b.body_end; have_moov = true; break; }
        pos = b.body_end;
    }
    if (!have_moov) {
        l_error("video: no moov box found in %s", path);
        sceIoClose(mp4->fd);
        mp4->fd = -1;
        return false;
    }

    for (uint64_t pos = moov_off; pos < moov_end; ) {
        Mp4Box b;
        if (!mp4_read_box(mp4->fd, pos, moov_end, &b)) break;
        if (b.type == MP4_FOURCC('t', 'r', 'a', 'k'))
            mp4_parse_trak(mp4->fd, b.body_off, b.body_end, &mp4->track[MP4_STREAM_VIDEO], &mp4->track[MP4_STREAM_AUDIO]);
        pos = b.body_end;
    }

    if (!mp4->track[MP4_STREAM_VIDEO].present) {
        l_error("video: no usable video track parsed from %s", path);
        sceIoClose(mp4->fd);
        mp4->fd = -1;
        return false;
    }
    return true;
}

// Next packet in file order across both tracks (whichever track's next
// sample sits at the lower file offset), matching how the file is actually
// interleaved -- reads never jump backwards. Returns false at EOF on both.
static bool mp4_next_packet(Mp4File *mp4, AVPacket *pkt, int *stream_index) {
    Mp4Track *v = &mp4->track[MP4_STREAM_VIDEO];
    Mp4Track *a = &mp4->track[MP4_STREAM_AUDIO];
    bool v_left = v->present && v->next < v->sample_count;
    bool a_left = a->present && a->next < a->sample_count;
    if (!v_left && !a_left)
        return false;

    bool take_video = (v_left && a_left) ? (v->samples[v->next].offset <= a->samples[a->next].offset) : v_left;

    Mp4Track *t = take_video ? v : a;
    Mp4Sample *s = &t->samples[t->next++];

    if (av_new_packet(pkt, (int) s->size) < 0)
        return false;
    if (sceIoLseek(mp4->fd, (SceOff) s->offset, SCE_SEEK_SET) < 0 ||
        sceIoRead(mp4->fd, pkt->data, (SceSize) s->size) != (int) s->size) {
        l_error("video: sceIoRead failed for a %u-byte sample at offset %llu",
                s->size, (unsigned long long) s->offset);
        av_packet_unref(pkt);
        return false;
    }
    pkt->pts = s->dts + s->cts_delta;
    pkt->dts = s->dts;
    *stream_index = take_video ? MP4_STREAM_VIDEO : MP4_STREAM_AUDIO;
    return true;
}

static void mp4_close(Mp4File *mp4) {
    if (mp4->track[MP4_STREAM_VIDEO].extradata) { av_free(mp4->track[MP4_STREAM_VIDEO].extradata); mp4->track[MP4_STREAM_VIDEO].extradata = NULL; }
    if (mp4->track[MP4_STREAM_AUDIO].extradata) { av_free(mp4->track[MP4_STREAM_AUDIO].extradata); mp4->track[MP4_STREAM_AUDIO].extradata = NULL; }
    if (mp4->track[MP4_STREAM_VIDEO].samples) { av_free(mp4->track[MP4_STREAM_VIDEO].samples); mp4->track[MP4_STREAM_VIDEO].samples = NULL; }
    if (mp4->track[MP4_STREAM_AUDIO].samples) { av_free(mp4->track[MP4_STREAM_AUDIO].samples); mp4->track[MP4_STREAM_AUDIO].samples = NULL; }
    if (mp4->fd >= 0) {
        sceIoClose(mp4->fd);
        mp4->fd = -1;
    }
}

// ---------------------------------------------------------------------------
// NEON YUV420P -> RGB565
// ---------------------------------------------------------------------------

/**
 * @brief Planar YUV420P (3 separate Y/U/V planes, FFmpeg's native decode
 * output for this codec) to RGB565, using ARM NEON intrinsics.
 *
 * Takes a stride (`AVFrame::linesize`) per plane, separate from the visible
 * width -- FFmpeg decode buffers are commonly padded/aligned wider than the
 * actual frame, unlike SceAvPlayer's exactly-sized buffers.
 *
 * Assumes even `w` and `h` (it walks two rows and two columns at a time);
 * callers round both down to even, which costs nothing on the 800x480 and
 * 848x480 assets this game actually ships.
 */
static int CV_R[256];
static int CV_G[256];
static int CU_G[256];
static int CU_B[256];
static unsigned char clip_table[768];
static bool tables_init = false;

static void init_yuv_tables() {
    if (tables_init) return;
    for (int i = 0; i < 256; i++) {
        int V = i - 128;
        int U = i - 128;
        CV_R[i] = (91881 * V) >> 16;
        CV_G[i] = (46802 * V) >> 16;
        CU_G[i] = (22554 * U) >> 16;
        CU_B[i] = (116130 * U) >> 16;
    }
    for (int i = 0; i < 768; i++) {
        int v = i - 256;
        clip_table[i] = (v < 0) ? 0 : ((v > 255) ? 255 : v);
    }
    tables_init = true;
}

#define CLIP(X) (clip_table[(X) + 256])

static inline void store_rgb565_8(unsigned short *dst, uint8x8_t r, uint8x8_t g, uint8x8_t b) {
    uint16x8_t rw = vmovl_u8(r);
    uint16x8_t gw = vmovl_u8(g);
    uint16x8_t bw = vmovl_u8(b);
    uint16x8_t rr = vshlq_n_u16(vandq_u16(rw, vdupq_n_u16(0xF8)), 8);
    uint16x8_t gg = vshlq_n_u16(vandq_u16(gw, vdupq_n_u16(0xFC)), 3);
    uint16x8_t bb = vshrq_n_u16(bw, 3);
    vst1q_u16((uint16_t *) dst, vorrq_u16(vorrq_u16(rr, gg), bb));
}

static void yuv420p_planar_to_rgb565(const unsigned char *yPlane, int yStride,
                                      const unsigned char *uPlane, int uStride,
                                      const unsigned char *vPlane, int vStride,
                                      unsigned w, unsigned h, unsigned short *dst) {
    init_yuv_tables();
    for (unsigned y = 0; y < h; y += 2) {
        const unsigned char *yrow0 = yPlane + (size_t) y * yStride;
        const unsigned char *yrow1 = yrow0 + yStride;
        const unsigned char *urow = uPlane + (size_t) (y / 2) * uStride;
        const unsigned char *vrow = vPlane + (size_t) (y / 2) * vStride;
        unsigned short *drow0 = dst + (size_t) y * w;
        unsigned short *drow1 = drow0 + w;

        unsigned x = 0;
        // Each NEON pass covers 16 luma pixels using 8 chroma samples (U and
        // V are already separate planes at half width, no deinterleave
        // needed -- simpler than the NV12 path SceAvPlayer needed).
        //
        // Bug #25 (log #069): chroma math in Q7 fixed point, staying in
        // int16 throughout. The previous version widened to int32 and used
        // Q16 constants (3 widen + 6-14 ALU ops per 8 chroma for G alone,
        // with 5-6-deep dependent chains through vmul->vshr->vmovn->combine
        // ->zip that stall the Cortex-A9's NEON pipeline back to back).
        // Q7 fits int16 with room to spare -- worst case 227*-128 = -29056,
        // inside [-32768, 32767] -- so each channel is one vmul + one
        // rounding shift, 3 dependent steps total (sub -> mul -> rshr):
        //
        //   R = Y + 1.402*V   -> (179*Vc + 64) >> 7    (179/128 = 1.3984)
        //   G = Y - 0.344*U - 0.714*V -> -(((44*Uc+64)>>7) + ((91*Vc+64)>>7))
        //   B = Y + 1.772*U   -> (227*Uc + 64) >> 7    (227/128 = 1.7734)
        //
        // Coefficient error vs the Q16 version is < 0.004 LSB -- invisible
        // once quantized to RGB565's 5/6 bits. vrshrq_n_s16 (VRSHR) folds
        // the +64 rounding into the shift. The Y-side (add + saturating
        // narrow + pack) is unchanged.
        for (; x + 16 <= w; x += 16) {
            uint8x8_t u8 = vld1_u8(urow + x / 2);
            uint8x8_t v8 = vld1_u8(vrow + x / 2);
            int16x8_t Uc = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(u8)), vdupq_n_s16(128));
            int16x8_t Vc = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(v8)), vdupq_n_s16(128));

            int16x8_t r_add8 = vrshrq_n_s16(vmulq_n_s16(Vc, 179), 7);
            int16x8_t g_add8 = vnegq_s16(vaddq_s16(vrshrq_n_s16(vmulq_n_s16(Uc, 44), 7),
                                                  vrshrq_n_s16(vmulq_n_s16(Vc, 91), 7)));
            int16x8_t b_add8 = vrshrq_n_s16(vmulq_n_s16(Uc, 227), 7);

            int16x8x2_t r_dup = vzipq_s16(r_add8, r_add8);
            int16x8x2_t g_dup = vzipq_s16(g_add8, g_add8);
            int16x8x2_t b_dup = vzipq_s16(b_add8, b_add8);

            for (int half = 0; half < 2; half++) {
                const unsigned char *yr0 = yrow0 + x + half * 8;
                const unsigned char *yr1 = yrow1 + x + half * 8;
                int16x8_t r_add = half == 0 ? r_dup.val[0] : r_dup.val[1];
                int16x8_t g_add = half == 0 ? g_dup.val[0] : g_dup.val[1];
                int16x8_t b_add = half == 0 ? b_dup.val[0] : b_dup.val[1];

                int16x8_t Y0 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(yr0)));
                int16x8_t Y1 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(yr1)));

                uint8x8_t r0 = vqmovun_s16(vaddq_s16(Y0, r_add));
                uint8x8_t g0 = vqmovun_s16(vaddq_s16(Y0, g_add));
                uint8x8_t b0 = vqmovun_s16(vaddq_s16(Y0, b_add));
                uint8x8_t r1 = vqmovun_s16(vaddq_s16(Y1, r_add));
                uint8x8_t g1 = vqmovun_s16(vaddq_s16(Y1, g_add));
                uint8x8_t b1 = vqmovun_s16(vaddq_s16(Y1, b_add));

                store_rgb565_8(drow0 + x + half * 8, r0, g0, b0);
                store_rgb565_8(drow1 + x + half * 8, r1, g1, b1);
            }
        }

        for (; x < w; x += 2) {
            unsigned char U = urow[x / 2];
            unsigned char V = vrow[x / 2];

            int r_add = CV_R[V];
            int g_add = -(CU_G[U] + CV_G[V]);
            int b_add = CU_B[U];

            int Y00 = yrow0[x];
            unsigned char r00 = CLIP(Y00 + r_add), g00 = CLIP(Y00 + g_add), b00 = CLIP(Y00 + b_add);
            drow0[x] = (unsigned short) (((r00 & 0xF8) << 8) | ((g00 & 0xFC) << 3) | (b00 >> 3));

            int Y01 = yrow0[x + 1];
            unsigned char r01 = CLIP(Y01 + r_add), g01 = CLIP(Y01 + g_add), b01 = CLIP(Y01 + b_add);
            drow0[x + 1] = (unsigned short) (((r01 & 0xF8) << 8) | ((g01 & 0xFC) << 3) | (b01 >> 3));

            int Y10 = yrow1[x];
            unsigned char r10 = CLIP(Y10 + r_add), g10 = CLIP(Y10 + g_add), b10 = CLIP(Y10 + b_add);
            drow1[x] = (unsigned short) (((r10 & 0xF8) << 8) | ((g10 & 0xFC) << 3) | (b10 >> 3));

            int Y11 = yrow1[x + 1];
            unsigned char r11 = CLIP(Y11 + r_add), g11 = CLIP(Y11 + g_add), b11 = CLIP(Y11 + b_add);
            drow1[x + 1] = (unsigned short) (((r11 & 0xF8) << 8) | ((g11 & 0xFC) << 3) | (b11 >> 3));
        }
    }
}

/*
 * One-time startup microbenchmark for Bug #25 (see port_progress.md and the
 * VIDEO_DECODE_THREADS comment above): log #067/#068 both measured the NEON
 * conversion above at ~90-98ms/frame for an 800x480 frame, on a CPU confirmed
 * at 444MHz -- roughly 15-20x slower than a rough instruction-count estimate
 * for this code. Dropping VIDEO_DECODE_THREADS to 1 (removing libavcodec's
 * own unpinned thread pool, the leading suspect) made no measurable
 * difference, which rules out scheduler contention with *that* pool as the
 * cause. This benchmark isolates the conversion routine completely from the
 * live decode/ring/GL pipeline -- synthetic buffers, no threads, no mutexes,
 * no libavcodec involved at all -- to settle whether the cost is intrinsic to
 * this routine on this hardware (real compute/bandwidth limit -- expect the
 * benchmark to reproduce ~90ms too) or specific to something about the live
 * decode thread's context (expect the benchmark to run fast, meaning the
 * live number is still an artifact of something -- e.g. the other threads
 * that DO remain: the audio thread, or contention with the render thread
 * itself). A plain memcpy over the same byte count is logged alongside as a
 * hardware bandwidth reference point either way.
 */
static void video_bench_convert(unsigned w, unsigned h, int runs) {
    const unsigned ySize = w * h, cSize = (w / 2) * (h / 2);
    const unsigned dstSize = w * h * (unsigned) sizeof(unsigned short);

    unsigned char *y = (unsigned char *) malloc(ySize);
    unsigned char *u = (unsigned char *) malloc(cSize);
    unsigned char *v = (unsigned char *) malloc(cSize);
    unsigned short *dst = (unsigned short *) malloc(dstSize);
    unsigned char *copySrc = (unsigned char *) malloc(dstSize);
    unsigned char *copyDst = (unsigned char *) malloc(dstSize);
    if (!y || !u || !v || !dst || !copySrc || !copyDst) {
        l_warn("video: startup benchmark skipped (out of memory)");
        goto out;
    }
    // Content doesn't matter for timing -- fill with something non-zero so
    // no fast-path/zero-page shortcut in any layer could skew the result.
    memset(y, 0x55, ySize);
    memset(u, 0x66, cSize);
    memset(v, 0x77, cSize);
    memset(copySrc, 0x88, dstSize);

    {
        uint64_t t0 = (uint64_t) now_us();
        for (int i = 0; i < runs; i++)
            yuv420p_planar_to_rgb565(y, (int) w, u, (int) (w / 2), v, (int) (w / 2), w, h, dst);
        uint64_t convert_us = (uint64_t) now_us() - t0;

        t0 = (uint64_t) now_us();
        for (int i = 0; i < runs; i++)
            memcpy(copyDst, copySrc, dstSize);
        uint64_t memcpy_us = (uint64_t) now_us() - t0;

        l_info("video: startup benchmark (%ux%u, %d runs, isolated from decode/ring/GL): "
               "yuv420p_planar_to_rgb565=%.1fms/call, memcpy(%u bytes)=%.1fms/call (%.0f MB/s)",
               w, h, runs,
               (double) convert_us / 1000.0 / runs,
               dstSize, (double) memcpy_us / 1000.0 / runs,
               (double) dstSize * runs / ((double) memcpy_us / 1000000.0) / (1024.0 * 1024.0));
    }

out:
    free(y); free(u); free(v); free(dst); free(copySrc); free(copyDst);
}

static void video_log_startup_benchmark(void) {
    // Both sizes: full (container resolution, pre-Bug-#25-fix reference) and
    // half (what lowres=1 actually decodes to -- the number the live
    // yuv_convert average should now be compared against).
    video_bench_convert(800, 480, 8);
    video_bench_convert(400, 240, 8);
}

// ---------------------------------------------------------------------------
// Playback state shared by the three threads
// ---------------------------------------------------------------------------

struct FrameSlot {
    unsigned short *rgb;
    unsigned        cap;      // bytes actually allocated
    unsigned        w, h;
    int64_t         pts_us;
};

static struct Playback {
    Mp4File          mp4;
    AVCodecContext  *vctx;
    AVCodecContext  *actx;
    SwrContext      *swr;
    AVRational       vtb;
    AVRational       atb;
    int64_t          frame_period_us;

    /* Finished frames: written only by the decode thread at `tail`, read
     * only by the render thread at `head`; `count` is the only field both
     * touch, so it is the only one the mutex has to cover. */
    FrameSlot        slots[VIDEO_FRAME_SLOTS];
    int              nslots;
    int              head, tail, count;
    pthread_mutex_t  ring_lock;

    /* Compressed audio packets: decode thread -> audio thread. */
    AVPacket        *aq[AUDIO_PKT_QUEUE_CAP];
    int              aq_head, aq_tail, aq_count;
    volatile bool    aq_eof;
    pthread_mutex_t  aq_lock;

    /* Audio output and the clock derived from it. */
    int               aport;
    int               arate;
    int               achannels;
    volatile uint32_t aplayed;      // frames the hardware has finished playing
    int64_t           abase_us;     // pts of the very first sample submitted
    volatile bool     aclock_valid;

    /* Lifecycle flags. Single-writer, aligned 32-bit -- no lock needed. */
    volatile bool    quit;          // skip/teardown requested
    volatile bool    decode_done;   // decode thread pushed its last frame
    volatile bool    audio_done;    // audio thread drained everything
    volatile bool    presenting;    // render thread has started its clock
    int64_t          wall_base_us;

    /* Stats, for the one summary line at the end. */
    int              decoded, presented, dropped_late;
    uint64_t         decode_us, convert_us, upload_us, draw_us;
} P;

/**
 * @brief Current playback position, in microseconds from the start of the file.
 *
 * Audio-mastered whenever there is audio: `aplayed` counts frames the audio
 * hardware has actually finished, so this clock runs at exactly the rate the
 * sound is coming out of the speakers. Video pinned to it cannot drift, and
 * if decode falls behind, audio playback slows too and video follows -- both
 * stay together instead of desynchronising. Falls back to wall time when
 * there is no audio stream, when the audio port could not be opened, or when
 * the watchdog in video_play() finds the audio clock stalled.
 */
static int64_t playback_clock_us(void) {
    if (P.aclock_valid) {
        uint32_t played = P.aplayed;
        return P.abase_us + (int64_t) played * 1000000LL / (int64_t) P.arate;
    }
    return now_us() - P.wall_base_us;
}

// ---------------------------------------------------------------------------
// Frame ring (decode thread -> render thread)
// ---------------------------------------------------------------------------

static int ring_count(void) {
    pthread_mutex_lock(&P.ring_lock);
    int c = P.count;
    pthread_mutex_unlock(&P.ring_lock);
    return c;
}

/**
 * @brief Converts one decoded frame into the next free ring slot.
 *
 * Blocks -- polling, never a `pthread_cond_t` (see the file header) -- while
 * the ring is full. That block is deliberate: it is what stops decode from
 * running arbitrarily far ahead of playback, and it is bounded by one frame
 * period because the render thread frees a slot every time it presents.
 * Returns false if playback is being torn down.
 */
static bool ring_push(const AVFrame *f, int64_t pts_us) {
    // The NEON converter walks 2 rows / 2 columns at a time.
    unsigned w = ((unsigned) f->width) & ~1u;
    unsigned h = ((unsigned) f->height) & ~1u;
    if (!w || !h)
        return false;
    unsigned need = w * h * (unsigned) sizeof(unsigned short);

    while (ring_count() >= P.nslots) {
        if (P.quit)
            return false;
        sceKernelDelayThread(1000);
    }
    if (P.quit)
        return false;

    FrameSlot *s = &P.slots[P.tail];
    if (s->cap < need) {
        free(s->rgb);
        s->rgb = (unsigned short *) malloc(need);
        s->cap = s->rgb ? need : 0;
    }
    if (!s->rgb) {
        l_error("video: out of memory for a %ux%u RGB565 frame slot", w, h);
        return false;
    }

    uint64_t t0 = (uint64_t) now_us();
    yuv420p_planar_to_rgb565(f->data[0], f->linesize[0],
                              f->data[1], f->linesize[1],
                              f->data[2], f->linesize[2],
                              w, h, s->rgb);
    P.convert_us += (uint64_t) now_us() - t0;

    s->w = w;
    s->h = h;
    s->pts_us = pts_us;

    pthread_mutex_lock(&P.ring_lock);
    P.tail = (P.tail + 1) % P.nslots;
    P.count++;
    pthread_mutex_unlock(&P.ring_lock);
    return true;
}

// ---------------------------------------------------------------------------
// Compressed audio packet queue (decode thread -> audio thread)
// ---------------------------------------------------------------------------

/**
 * @brief Hands one audio packet to the audio thread, taking ownership of it.
 *
 * Waits while the queue is full, which just means audio is comfortably ahead;
 * the audio thread drains at real time, so the wait is short and bounded.
 * Returns false (caller frees the packet) only if playback is ending.
 */
static bool aq_push(AVPacket *pkt) {
    for (;;) {
        pthread_mutex_lock(&P.aq_lock);
        bool full = (P.aq_count >= AUDIO_PKT_QUEUE_CAP);
        if (!full) {
            P.aq[P.aq_tail] = pkt;
            P.aq_tail = (P.aq_tail + 1) % AUDIO_PKT_QUEUE_CAP;
            P.aq_count++;
        }
        pthread_mutex_unlock(&P.aq_lock);
        if (!full)
            return true;
        if (P.quit)
            return false;
        sceKernelDelayThread(1000);
    }
}

static AVPacket *aq_pop(void) {
    AVPacket *pkt = NULL;
    pthread_mutex_lock(&P.aq_lock);
    if (P.aq_count > 0) {
        pkt = P.aq[P.aq_head];
        P.aq_head = (P.aq_head + 1) % AUDIO_PKT_QUEUE_CAP;
        P.aq_count--;
    }
    pthread_mutex_unlock(&P.aq_lock);
    return pkt;
}

static void aq_drain_and_free(void) {
    AVPacket *pkt;
    while ((pkt = aq_pop()) != NULL)
        av_packet_free(&pkt);
}

// ---------------------------------------------------------------------------
// GL: draw one frame
// ---------------------------------------------------------------------------

static GLuint gVideoTex = 0;
static unsigned gVideoTexW = 0;
static unsigned gVideoTexH = 0;

/*
 * NOT the real 960x544 Vita screen, and NOT the 800x480 the engine itself is
 * told the screen is -- the downsample FBO glutil.c's gl_init() sets up
 * (OFFSCREEN_W/H, defined in glutil.h so this stays in sync automatically)
 * stays bound for the entire program, including while this cutscene player
 * is drawing (nothing here ever rebinds a framebuffer). A viewport/quad
 * sized for anything other than the FBO's actual attachment size would draw
 * past it and get clipped by the GPU, cropping the video into a corner --
 * the same bug class already fixed for the game's own rendering (see Bug
 * #13 in port_progress.md). gl_swap() upscale-blits this FBO's full content
 * (video included) onto the real screen once per frame, so targeting
 * OFFSCREEN_W/H here is correct regardless of what that size is.
 *
 * 720x432 is the same 5:3 as the 800x480 trailer, so that one fills the
 * frame exactly with no letterboxing; the 848x480 in-race videos are wider
 * and get letterboxed by the aspect fit below.
 */
#define VIDEO_TARGET_W OFFSCREEN_W
#define VIDEO_TARGET_H OFFSCREEN_H

static bool gFirstDrawLogged = false;
#define FIRST_DRAW_LOG(...) do { if (!gFirstDrawLogged) l_info(__VA_ARGS__); } while (0)

/*
 * Draws one decoded video frame (already CPU-converted to RGB565 by
 * yuv420p_planar_to_rgb565() on the decode thread) as a letterboxed quad,
 * using PLAIN GLES1.1 fixed-function texturing -- deliberately NOT a custom
 * GLSL program with generic vertex attribute arrays, which an earlier
 * version of this function did (glUseProgram(customProgram) +
 * glVertexAttribPointer on attribute locations 0/1, doing the YUV->RGB
 * conversion in a fragment shader instead of on the CPU).
 *
 * That GLES2 version caused a real, confirmed-on-hardware regression: the
 * first time it ever actually ran, every fixed-function draw for the rest
 * of the program's life -- the loading screen, the title screen -- came out
 * solid white, while the menu (reached later, evidently through a
 * different code path or one that resets more GL state itself) still
 * worked. `libasphalt5.so` is GLES1.1 fixed-function only and runs through
 * vitaGL, a translation layer that implements the fixed-function pipeline
 * via its OWN internally-managed shader machinery, layered UNDER the same
 * `glUseProgram`/vertex-attribute API surface real custom shaders use.
 * Keep this on the same plain fixed-function path as
 * `gl_blit_downsample_to_screen()` in glutil.c -- the other quad blit in
 * this codebase, hardware-proven every single frame -- rather than
 * reintroducing that risk. (Dungeon-Hunter-2-vita's video.cpp does convert
 * YUV on the GPU with a GLES2 program and two LUMINANCE/LUMINANCE_ALPHA
 * textures, which is cheaper on CPU; it is the one thing from that port
 * deliberately not adopted here, for the reason above. The NEON conversion
 * this replaces it with costs a few ms on a core that is otherwise idle.)
 */
static void draw_video_frame(const unsigned short *rgb565, unsigned w, unsigned h) {
    FIRST_DRAW_LOG("video: draw_video_frame ENTER (%ux%u)", w, h);

    uint64_t t_upload0 = (uint64_t) now_us();

    if (!gVideoTex || gVideoTexW != w || gVideoTexH != h) {
        FIRST_DRAW_LOG("video: creating texture storage...");
        if (!gVideoTex) glGenTextures(1, &gVideoTex);
        glBindTexture(GL_TEXTURE_2D, gVideoTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        FIRST_DRAW_LOG("video: about to glTexImage2D (%ux%u, RGB565)...", w, h);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (GLsizei) w, (GLsizei) h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
        FIRST_DRAW_LOG("video: glTexImage2D returned (err=0x%04x)", glGetError());
        gVideoTexW = w;
        gVideoTexH = h;
    }
    glBindTexture(GL_TEXTURE_2D, gVideoTex);
    FIRST_DRAW_LOG("video: about to glTexSubImage2D...");
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei) w, (GLsizei) h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, rgb565);
    FIRST_DRAW_LOG("video: glTexSubImage2D returned (err=0x%04x)", glGetError());

    uint64_t t_draw0 = (uint64_t) now_us();
    P.upload_us += t_draw0 - t_upload0;

    float srcAspect = (float) w / (float) h;
    float dstAspect = (float) VIDEO_TARGET_W / (float) VIDEO_TARGET_H;
    float qx0 = 0, qy0 = 0, qx1 = VIDEO_TARGET_W, qy1 = VIDEO_TARGET_H;
    if (srcAspect > dstAspect) {
        float qh = VIDEO_TARGET_W / srcAspect;
        qy0 = (VIDEO_TARGET_H - qh) / 2.0f;
        qy1 = qy0 + qh;
    } else {
        float qw = VIDEO_TARGET_H * srcAspect;
        qx0 = (VIDEO_TARGET_W - qw) / 2.0f;
        qx1 = qx0 + qw;
    }

    float nx0 = qx0 / VIDEO_TARGET_W * 2.0f - 1.0f;
    float nx1 = qx1 / VIDEO_TARGET_W * 2.0f - 1.0f;
    float ny0 = 1.0f - qy0 / VIDEO_TARGET_H * 2.0f;
    float ny1 = 1.0f - qy1 / VIDEO_TARGET_H * 2.0f;

    // Static storage, not a stack array -- client-side vertex pointers are
    // only guaranteed read AT glDrawArrays in a spec-strict implementation;
    // gl_blit_downsample_to_screen() in glutil.c uses `static const` for the
    // same reason. Contents vary per call (aspect ratio depends on the
    // video), so this can't also be `const` the way that fixed fullscreen
    // quad is. Only ever touched from the render thread.
    static GLfloat verts[8];
    verts[0] = nx0; verts[1] = ny0;
    verts[2] = nx1; verts[3] = ny0;
    verts[4] = nx0; verts[5] = ny1;
    verts[6] = nx1; verts[7] = ny1;
    static const GLfloat uvs[8] = {
        0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f,  1.0f, 1.0f,
    };

    FIRST_DRAW_LOG("video: about to save GL state...");
    GLboolean savedBlend = glIsEnabled(GL_BLEND);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean savedScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean savedCull = glIsEnabled(GL_CULL_FACE);
    GLboolean savedTex2D = glIsEnabled(GL_TEXTURE_2D);
    GLboolean savedVertexArray = glIsEnabled(GL_VERTEX_ARRAY);
    GLboolean savedTexCoordArray = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
    GLint savedViewport[4];
    glGetIntegerv(GL_VIEWPORT, savedViewport);
    GLint savedTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex);
    FIRST_DRAW_LOG("video: GL state saved (blend=%d depth=%d scissor=%d cull=%d tex=%d)",
                   savedBlend, savedDepthTest, savedScissor, savedCull, savedTex);

    glViewport(0, 0, VIDEO_TARGET_W, VIDEO_TARGET_H);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    FIRST_DRAW_LOG("video: viewport/disables done");

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, gVideoTex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);
    FIRST_DRAW_LOG("video: about to glDrawArrays...");
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    FIRST_DRAW_LOG("video: glDrawArrays returned (err=0x%04x)", glGetError());
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    if (!gFirstDrawLogged) {
        unsigned char pixel[4] = {0, 0, 0, 0};
        glReadPixels(VIDEO_TARGET_W / 2, VIDEO_TARGET_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        l_info("[video_diag] framebuffer readback at center, right after glDrawArrays: rgba=%u,%u,%u,%u (err=0x%04x)",
               pixel[0], pixel[1], pixel[2], pixel[3], glGetError());
    }

    FIRST_DRAW_LOG("video: about to gl_swap()...");
    gl_swap();
    FIRST_DRAW_LOG("video: gl_swap() returned -- first frame fully presented");

    glBindTexture(GL_TEXTURE_2D, (GLuint) savedTex);
    if (savedBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (savedDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (savedScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (savedCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);

    // Restore client arrays
    if (savedVertexArray) glEnableClientState(GL_VERTEX_ARRAY); else glDisableClientState(GL_VERTEX_ARRAY);
    if (savedTexCoordArray) glEnableClientState(GL_TEXTURE_COORD_ARRAY); else glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    // Engine expects texture env mode to be MODULATE by default (we forced REPLACE)
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    // We disabled texture 2D at the end of drawing, restore it to its original state
    if (savedTex2D) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);

    FIRST_DRAW_LOG("video: GL state restored");
    gFirstDrawLogged = true;

    P.draw_us += (uint64_t) now_us() - t_draw0;
}

// ---------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------

/**
 * @brief Decodes, resamples and plays the cutscene's audio track.
 *
 * Owns the whole audio path end to end, unlike the previous version where
 * the decode loop resampled inline and a helper thread only relayed finished
 * buffers to `sceAudioOutOutput()`. That split meant every blocking audio
 * submit stalled video decode; here the only thing that blocks on audio is
 * audio. `sceAudioOutOutput()` returning is also what advances `P.aplayed`,
 * which is the clock the whole video side is paced against.
 *
 * Two output blocks alternating: a Vita audio port consumes the buffer
 * handed to it while the call for the *next* block is what blocks, so the
 * buffer passed last must stay untouched until then.
 */
static void *cutscene_audio_thread(void *arg) {
    (void) arg;
    // Same placement sceKernelCreateThread() would have given it (see
    // audio.cpp: mixer on core 1, heavy loader on core 2). Realtime priority
    // so a decode spike can never underrun the audio port.
    sceKernelChangeThreadPriority(0, 0x40);
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);

    const int      ch         = P.achannels;
    const unsigned blockBytes = AUDIO_GRAIN * (unsigned) ch * sizeof(int16_t);

    int16_t *blocks[2] = { (int16_t *) memalign(64, blockBytes),
                           (int16_t *) memalign(64, blockBytes) };
    unsigned accCapFrames = AUDIO_GRAIN * 4;
    int16_t *acc = (int16_t *) malloc((size_t) accCapFrames * ch * sizeof(int16_t));
    AVFrame *frame = av_frame_alloc();

    if (!blocks[0] || !blocks[1] || !acc || !frame) {
        l_error("video: out of memory for cutscene audio buffers -- cutscene will be silent");
        goto done;
    }

    {
        unsigned accFrames = 0;
        int      blk = 0;
        unsigned blocksOut = 0;
        bool     eof = false;
        int      frames = 0;

        // Emits every whole block currently sitting in the accumulator.
        // `blocksOut > 0` before crediting: sceAudioOutOutput() returns once
        // the PREVIOUS buffer has finished playing, so after the Nth call
        // exactly (N-1) blocks have actually been heard.
        auto flush_blocks = [&](void) {
            while (accFrames >= AUDIO_GRAIN) {
                memcpy(blocks[blk], acc, blockBytes);
                accFrames -= AUDIO_GRAIN;
                if (accFrames)
                    memmove(acc, acc + (size_t) AUDIO_GRAIN * ch,
                            (size_t) accFrames * ch * sizeof(int16_t));
                sceAudioOutOutput(P.aport, blocks[blk]);
                if (blocksOut > 0)
                    P.aplayed += AUDIO_GRAIN;
                blocksOut++;
                blk ^= 1;
                if (P.quit)
                    return;
            }
        };

        // Appends one decoded frame's worth of resampled S16 to the accumulator.
        auto resample_into_acc = [&](AVFrame *f) {
            int outMax = swr_get_out_samples(P.swr, f->nb_samples);
            if (outMax < 0)
                outMax = f->nb_samples;
            if (accFrames + (unsigned) outMax > accCapFrames) {
                unsigned newCap = accFrames + (unsigned) outMax + AUDIO_GRAIN;
                int16_t *bigger = (int16_t *) realloc(acc, (size_t) newCap * ch * sizeof(int16_t));
                if (!bigger)
                    return;
                acc = bigger;
                accCapFrames = newCap;
            }
            uint8_t *dst[1] = { (uint8_t *) (acc + (size_t) accFrames * ch) };
            int got = swr_convert(P.swr, dst, outMax,
                                   (const uint8_t **) f->extended_data, f->nb_samples);
            if (got > 0)
                accFrames += (unsigned) got;
        };

        while (!P.quit) {
            AVPacket *pkt = aq_pop();
            if (!pkt) {
                if (P.aq_eof) {
                    // Flush the decoder, then the accumulator.
                    avcodec_send_packet(P.actx, NULL);
                    eof = true;
                } else {
                    sceKernelDelayThread(1000);
                    continue;
                }
            } else {
                avcodec_send_packet(P.actx, pkt);
                av_packet_free(&pkt);
            }

            for (;;) {
                int rc = avcodec_receive_frame(P.actx, frame);
                if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
                    break;
                if (rc < 0) {
                    l_warn("video: avcodec_receive_frame(audio) error 0x%08x", rc);
                    break;
                }
                if (++frames == 1) {
                    // Anchor the clock on the first real sample's timestamp
                    // (0 for every asset this game ships, but read rather
                    // than assumed) BEFORE publishing it as usable.
                    P.abase_us = (frame->pts != AV_NOPTS_VALUE)
                                     ? av_rescale_q(frame->pts, P.atb, kUsTimeBase) : 0;
                    __sync_synchronize();
                    P.aclock_valid = true;
                    l_info("video: first audio frame decoded (out %d Hz, %d ch, base=%lldus)",
                           P.arate, ch, (long long) P.abase_us);
                }
                resample_into_acc(frame);
                av_frame_unref(frame);
                flush_blocks();
                if (P.quit)
                    break;
            }

            if (eof)
                break;
        }

        if (!P.quit) {
            // Pad the tail to a whole block, then push one block of silence
            // so the last real block is actually heard before teardown.
            if (accFrames > 0 && accFrames < AUDIO_GRAIN) {
                memset(acc + (size_t) accFrames * ch, 0,
                       (size_t) (AUDIO_GRAIN - accFrames) * ch * sizeof(int16_t));
                accFrames = AUDIO_GRAIN;
            }
            flush_blocks();
            if (blocksOut > 0) {
                memset(blocks[blk], 0, blockBytes);
                sceAudioOutOutput(P.aport, blocks[blk]);
                P.aplayed += AUDIO_GRAIN;
            }
        }
    }

done:
    if (frame) av_frame_free(&frame);
    free(acc);
    free(blocks[0]);
    free(blocks[1]);
    P.audio_done = true;
    return NULL;
}

// ---------------------------------------------------------------------------
// Decode thread
// ---------------------------------------------------------------------------

static void handle_decoded_frame(AVFrame *f, int64_t *last_pts_us) {
    if (f->format != AV_PIX_FMT_YUV420P) {
        // Software mpeg4 decode should always hand back planar YUV420P
        // directly -- this project has no swscale conversion path wired up,
        // so anything else (an unusual profile, a hw pixel format slipping
        // through) is logged and dropped rather than misread as YUV420P and
        // drawn as garbage.
        l_warn("video: decoded frame has unexpected pix_fmt %d (expected YUV420P/%d) -- dropping frame",
               (int) f->format, (int) AV_PIX_FMT_YUV420P);
        return;
    }

    int64_t ts = (f->best_effort_timestamp != AV_NOPTS_VALUE) ? f->best_effort_timestamp : f->pts;
    int64_t pts_us;
    if (ts != AV_NOPTS_VALUE)
        pts_us = av_rescale_q(ts, P.vtb, kUsTimeBase);
    else
        pts_us = (*last_pts_us >= 0) ? *last_pts_us + P.frame_period_us : 0;
    *last_pts_us = pts_us;

    if (++P.decoded == 1)
        l_info("video: first video frame decoded (%dx%d)", f->width, f->height);

    if (P.presenting) {
        int64_t late = playback_clock_us() - pts_us;

        /*
         * Two levels of catching up, so a frame that can no longer be shown
         * on time costs as little as possible:
         *
         *  - More than ~0.4s behind: tell the decoder itself to stop fully
         *    reconstructing frames nothing else depends on (AVDISCARD_NONREF
         *    skips B-frames, which the 848x480 Advanced Simple Profile
         *    in-race videos do use; the Simple Profile trailer has none, so
         *    there it is simply a no-op). Reset as soon as we are back within
         *    0.1s so quality returns the moment it can.
         *  - Already past its slot by more than 1.5 frame periods: skip the
         *    NEON conversion and the ring slot entirely. The frame was
         *    decoded (it may be a reference for later ones) but converting,
         *    uploading, drawing and swapping it would only push us further
         *    behind for something the viewer would never see.
         */
        P.vctx->skip_frame = (late > 400000) ? AVDISCARD_NONREF
                             : (late < 100000 ? AVDISCARD_DEFAULT : P.vctx->skip_frame);
        if (late > P.frame_period_us + P.frame_period_us / 2) {
            P.dropped_late++;
            return;
        }
    }

    ring_push(f, pts_us);
}

/**
 * @brief Demuxes the file, decodes video, and routes audio packets onward.
 *
 * Canonical send/receive loop: drain every frame the decoder can produce
 * first, and only feed it another packet once it says it needs one -- the
 * shape libavcodec always expects regardless of thread_count (see
 * VIDEO_DECODE_THREADS), so this loop needs no changes if that ever goes
 * back up.
 */
static void *video_decode_thread(void *arg) {
    (void) arg;
    // Cores 1 and 2: keep the render thread's core to itself, but leave the
    // decoder two cores to move between. Set here, from inside the thread,
    // rather than at creation, because this has to be a real pthread (see
    // the file header) and libpthread has no portable affinity attribute.
    // Priority is below audio.cpp's mixer (0x40) on purpose -- video may
    // stutter under load, audio must not. FFmpeg's own frame threads were
    // created by avcodec_open2() on the render thread and keep the default
    // all-cores affinity, which is what lets them actually spread out.
    sceKernelChangeThreadPriority(0, 0x60);
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2);

    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frame = av_frame_alloc();
    int64_t   last_pts_us = -1;
    bool      eof = false;

    if (!pkt || !frame) {
        l_error("video: out of memory for decode thread packet/frame");
        goto done;
    }

    while (!P.quit) {
        uint64_t td0 = (uint64_t) now_us();
        int rc = avcodec_receive_frame(P.vctx, frame);
        P.decode_us += (uint64_t) now_us() - td0;
        if (rc == 0) {
            handle_decoded_frame(frame, &last_pts_us);
            av_frame_unref(frame);
            continue;
        }
        if (rc == AVERROR_EOF)
            break;  // flushed and fully drained: playback ended naturally
        if (rc != AVERROR(EAGAIN)) {
            l_warn("video: avcodec_receive_frame(video) error 0x%08x", rc);
            break;
        }
        if (eof)
            break;  // decoder wants input after a flush; nothing left to give

        int stream_index;
        if (!mp4_next_packet(&P.mp4, pkt, &stream_index)) {
            // Flush the video decoder (a NULL packet) so frames still held
            // inside it get decoded rather than silently dropped, and tell
            // the audio thread no more packets are coming.
            eof = true;
            td0 = (uint64_t) now_us();
            avcodec_send_packet(P.vctx, NULL);
            P.decode_us += (uint64_t) now_us() - td0;
            P.aq_eof = true;
            continue;
        }

        if (stream_index == MP4_STREAM_VIDEO) {
            td0 = (uint64_t) now_us();
            avcodec_send_packet(P.vctx, pkt);
            P.decode_us += (uint64_t) now_us() - td0;
            av_packet_unref(pkt);
        } else if (P.actx) {
            // Move (don't copy) the payload into a packet the audio thread owns.
            AVPacket *ap = av_packet_alloc();
            if (ap) {
                av_packet_move_ref(ap, pkt);
                if (!aq_push(ap))
                    av_packet_free(&ap);
            }
            av_packet_unref(pkt);
        } else {
            av_packet_unref(pkt);
        }
    }

done:
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    P.aq_eof = true;      // covers the early-exit paths above
    P.decode_done = true;
    return NULL;
}

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------

void video_init() {
    memset(&P, 0, sizeof(P));
    P.aport = -1;
    pthread_mutex_init(&P.ring_lock, NULL);
    pthread_mutex_init(&P.aq_lock, NULL);
    l_success("video: FFmpeg software decoder ready (%s).", av_version_info());
    video_log_startup_benchmark();
}

static void free_frame_slots(void) {
    for (int i = 0; i < VIDEO_FRAME_SLOTS; i++) {
        free(P.slots[i].rgb);
        P.slots[i].rgb = NULL;
        P.slots[i].cap = 0;
    }
}

void video_shutdown() {
    if (gVideoTex) {
        glDeleteTextures(1, &gVideoTex);
        gVideoTex = 0;
        gVideoTexW = 0;
        gVideoTexH = 0;
    }
    free_frame_slots();
}

/*
 * Resolves `name` (a bare filename, e.g. "A5_Ultimate_VNFS_2.mp4" --
 * confirmed via GS_TrailerMovie::Create() in the decompiled engine) against
 * the same set of candidate locations the port has always used for this.
 *
 * (A previous debugging pass had this force-substitute "A5_Ultimate_VNFS_2"
 * to the "_854" variant and try .mkv/.avi remuxes first -- a workaround for
 * the missing-mov-demuxer bug documented at the top of this file, not a
 * real requirement. Reverted now that the actual bug is fixed: the engine
 * always asks for this exact name, so it is opened directly.)
 */
static bool resolve_video_path(const char *name, char *path, size_t pathSize) {
    SceIoStat st;
    const char *prefixes[] = {
        DATA_PATH "%s",
        DATA_PATH "data/%s",
        DATA_PATH "data/%s.mp4",
        DATA_PATH "%s.mp4",
        DATA_PATH "files/%s",
        DATA_PATH "res/raw/%s",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        snprintf(path, pathSize, prefixes[i], name);
        if (sceIoGetstat(path, &st) >= 0)
            return true;
    }
    return false;
}

/*
 * Sample rates a Vita audio port will accept. Everything this game ships is
 * 44100 (trailer) or 22050 (in-race videos), both on the list, but resample
 * rather than fail if a rate ever turns up that the hardware won't take.
 */
static bool audio_rate_supported(int rate) {
    static const int kRates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
    for (size_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); i++)
        if (kRates[i] == rate)
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// video_play()
// ---------------------------------------------------------------------------

/**
 * @brief Plays a video file, blocking the calling (render) thread until it
 *        ends, is skipped, or fails.
 * @param name File name or relative path of the video cutscene to play.
 */
void video_play(const char *name) {
    if (!name) {
        l_warn("video: video_play() called with a null name, skipping");
        return;
    }

    char path[512];
    if (!resolve_video_path(name, path, sizeof(path))) {
        l_error("video: file not found for \"%s\" (searched DATA_PATH data/, files/, res/raw/)", name);
        return;
    }

    // Reset per-playback state, keeping the mutexes video_init() set up.
    P.vctx = NULL; P.actx = NULL; P.swr = NULL;
    memset(&P.mp4, 0, sizeof(P.mp4));
    P.mp4.fd = -1;
    P.head = P.tail = P.count = 0;
    P.aq_head = P.aq_tail = P.aq_count = 0;
    P.aq_eof = false;
    P.aport = -1;
    P.arate = 0; P.achannels = 0;
    P.aplayed = 0; P.abase_us = 0; P.aclock_valid = false;
    P.quit = false; P.decode_done = false; P.audio_done = false; P.presenting = false;
    P.decoded = P.presented = P.dropped_late = 0;
    P.decode_us = P.convert_us = P.upload_us = P.draw_us = 0;
    P.nslots = VIDEO_FRAME_SLOTS;
    P.frame_period_us = VIDEO_FRAME_PERIOD_US;

    pthread_t decodeThread, audioThread;
    bool decodeThreadUp = false, audioThreadUp = false;
    bool skipped = false;

    // ---- parse the MP4 box tree ourselves; libavformat is not involved
    // at all (see the file header for why) ----
    if (!mp4_open(path, &P.mp4))
        return;

    Mp4Track *vtrk = &P.mp4.track[MP4_STREAM_VIDEO];
    Mp4Track *atrk = &P.mp4.track[MP4_STREAM_AUDIO];

    // ---- video decoder, single-threaded (see VIDEO_DECODE_THREADS) ----
    {
        const AVCodec *codec = avcodec_find_decoder(vtrk->codec_id);
        if (!codec) {
            l_error("video: no software decoder registered for video codec id %d -- is the "
                    "vdpm ffmpeg build's decoder list missing mpeg4? (see CMakeLists.txt comment)",
                    (int) vtrk->codec_id);
        } else {
            P.vctx = avcodec_alloc_context3(codec);
            if (P.vctx) {
                P.vctx->width = vtrk->width;
                P.vctx->height = vtrk->height;
                if (vtrk->extradata) {
                    P.vctx->extradata = (uint8_t *) av_mallocz(vtrk->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
                    if (P.vctx->extradata) {
                        memcpy(P.vctx->extradata, vtrk->extradata, vtrk->extradata_size);
                        P.vctx->extradata_size = vtrk->extradata_size;
                    }
                }
                P.vctx->thread_count = VIDEO_DECODE_THREADS;
                // Permits speedups that are not strictly bit-exact. Harmless
                // for a cutscene, measurably cheaper per macroblock.
                P.vctx->flags2 |= AV_CODEC_FLAG2_FAST;
                /*
                 * Bug #25 (log #069): decode at half resolution. The startup
                 * benchmark isolated yuv420p_planar_to_rgb565() at 90.7ms for
                 * one 800x480 frame with no threads/GL/libavcodec involved --
                 * the cost is intrinsic to pushing 384k pixels through this
                 * CPU, not scheduler contention (VIDEO_DECODE_THREADS=1
                 * already ruled that out). Decoding at 1/2 x 1/2 (400x240 for
                 * the trailer, 424x240 for the in-race videos) quarters every
                 * pixel-proportional stage at once: the NEON conversion
                 * (~90ms -> ~22ms), the glTexSubImage2D upload (12.6ms ->
                 * ~3ms: 192KB instead of 768KB), and the MPEG-4 decode itself
                 * (~10ms -> ~4ms). The existing draw path already upscales
                 * with GL_LINEAR onto the 720x432 FBO, so no other change is
                 * needed; 400x240 is exactly the same 5:3 aspect as the
                 * target, 424x240 letterboxes as before. A skippable cutscene
                 * at 30fps soft beats a sharp one at 10fps.
                 *
                 * `lowres` is honored by the mpegvideo-family decoders (this
                 * game's MPEG-4 Part 2 included); must be set before
                 * avcodec_open2(). If a future FFmpeg build ever ignores it,
                 * playback still works -- frames just come back full-size and
                 * ring_push()/draw_video_frame() size themselves from
                 * f->width/height dynamically, only slower.
                 */
                P.vctx->lowres = 1;
                if (avcodec_open2(P.vctx, codec, NULL) < 0) {
                    l_error("video: avcodec_open2 failed for video stream");
                    avcodec_free_context(&P.vctx);
                }
            }
        }
    }
    if (!P.vctx) {
        l_error("video: no usable video decoder for %s, aborting playback", path);
        goto cleanup;
    }

    {
        AVRational vtb = { 1, (int) vtrk->timescale };
        P.vtb = vtb;
    }
    {
        // Fixed 30 fps presentation, like both reference ports (see
        // VIDEO_FRAME_PERIOD_US) -- the source rate isn't parsed at all,
        // nothing here needs it. Width/height logged are the DECODED size
        // (half of the container's with lowres=1, see above).
        P.frame_period_us = VIDEO_FRAME_PERIOD_US;
        l_success("video: playing %s -- %dx%d %s (lowres=%d), fixed 30 fps (period %lldus), %d decode thread(s)",
                  path, P.vctx->width, P.vctx->height, avcodec_get_name(P.vctx->codec_id),
                  P.vctx->lowres,
                  (long long) P.frame_period_us, P.vctx->thread_count);
    }

    // ---- audio decoder + resampler + output port ----
    if (atrk->present) {
        const AVCodec *codec = avcodec_find_decoder(atrk->codec_id);
        if (!codec) {
            l_warn("video: no software decoder registered for audio codec id %d -- cutscene will be silent",
                   (int) atrk->codec_id);
        } else {
            P.actx = avcodec_alloc_context3(codec);
            if (P.actx) {
                P.actx->sample_rate = atrk->sample_rate;
                av_channel_layout_default(&P.actx->ch_layout, atrk->channels > 0 ? atrk->channels : 2);
                if (atrk->extradata) {
                    P.actx->extradata = (uint8_t *) av_mallocz(atrk->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
                    if (P.actx->extradata) {
                        memcpy(P.actx->extradata, atrk->extradata, atrk->extradata_size);
                        P.actx->extradata_size = atrk->extradata_size;
                    }
                }
                P.actx->thread_count = 1;  // AAC is cheap; keep the cores for video
                if (avcodec_open2(P.actx, codec, NULL) < 0) {
                    l_warn("video: avcodec_open2 failed for audio stream -- cutscene will be silent");
                    avcodec_free_context(&P.actx);
                }
            }
        }
    }

    if (P.actx) {
        {
            AVRational atb = { 1, (int) atrk->timescale };
            P.atb = atb;
        }
        P.achannels = P.actx->ch_layout.nb_channels > 0 ? P.actx->ch_layout.nb_channels : 2;
        if (P.achannels > 2)
            P.achannels = 2;  // a Vita port is stereo at most
        P.arate = audio_rate_supported(P.actx->sample_rate) ? P.actx->sample_rate : 48000;
        if (P.arate != P.actx->sample_rate)
            l_info("video: source audio is %d Hz, which no Vita audio port accepts -- resampling to %d Hz",
                   P.actx->sample_rate, P.arate);

        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, P.achannels);
        int rc = swr_alloc_set_opts2(&P.swr, &outLayout, AV_SAMPLE_FMT_S16, P.arate,
                                      &P.actx->ch_layout, P.actx->sample_fmt, P.actx->sample_rate,
                                      0, NULL);
        av_channel_layout_uninit(&outLayout);
        if (rc < 0 || !P.swr || swr_init(P.swr) < 0) {
            l_warn("video: swr_alloc_set_opts2/swr_init failed -- cutscene will be silent");
            if (P.swr) { swr_free(&P.swr); P.swr = NULL; }
        }
    }

    if (P.swr) {
        SceAudioOutMode mode = (P.achannels >= 2) ? SCE_AUDIO_OUT_MODE_STEREO
                                                   : SCE_AUDIO_OUT_MODE_MONO;
        P.aport = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_VOICE, AUDIO_GRAIN, P.arate, mode);
        if (P.aport < 0) {
            l_warn("video: sceAudioOutOpenPort for cutscene audio failed (0x%08X) -- "
                   "cutscene audio disabled", (unsigned) P.aport);
            swr_free(&P.swr);
            P.swr = NULL;
        } else {
            l_info("video: cutscene audio port %d open (%d Hz, %d ch, %d frames/block)",
                   P.aport, P.arate, P.achannels, AUDIO_GRAIN);
        }
    }

    // ---- start the workers ----
    // pthread_create, not sceKernelCreateThread: FFmpeg's frame threading
    // waits on condition variables on whichever thread calls
    // avcodec_receive_frame(), and that is only safe from a thread
    // libpthread knows about (see the file header). Affinity/priority are
    // set from inside each thread instead.
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 512 * 1024);
        if (pthread_create(&decodeThread, &attr, video_decode_thread, NULL) == 0) {
            decodeThreadUp = true;
        } else {
            l_error("video: could not start the decode thread -- aborting playback");
        }
        pthread_attr_destroy(&attr);
    }
    if (!decodeThreadUp)
        goto cleanup;

    if (P.swr && P.aport >= 0) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        if (pthread_create(&audioThread, &attr, cutscene_audio_thread, NULL) == 0) {
            audioThreadUp = true;
        } else {
            l_warn("video: could not start the cutscene audio thread -- cutscene will be silent");
        }
        pthread_attr_destroy(&attr);
    }
    if (!audioThreadUp)
        P.aclock_valid = false;  // nothing will advance the audio clock: wall clock it is

    // ---- present, on this (the render/GL) thread ----
    {
        SceCtrlData pad_start;
        sceCtrlPeekBufferPositive(0, &pad_start, 1);
        uint32_t old_pad = pad_start.buttons;

        int64_t play_start = now_us();
        P.wall_base_us = play_start;
        __sync_synchronize();
        P.presenting = true;

        int64_t last_clk = -1;
        int64_t last_clk_move = play_start;
        int64_t last_present_us = 0;  // wall time of the last gl_swap; 0 = none yet

        l_info("video: present loop starting");

        while (!P.quit) {
            // Bug #26: same idle-timer reset as the main render loop -- this
            // present loop owns the render thread for the whole cutscene, so
            // the tick in main.c doesn't run until playback ends.
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
            SceCtrlData pad;
            sceCtrlPeekBufferPositive(0, &pad, 1);
            uint32_t pressed = pad.buttons & ~old_pad;
            old_pad = pad.buttons;
            if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_START)) {
                l_info("video: skipped by user button press! (pad=0x%08X)", (unsigned) pad.buttons);
                skipped = true;
                P.quit = true;
                break;
            }

            int64_t clk = playback_clock_us();
            int64_t nw = now_us();
            if (clk != last_clk) {
                last_clk = clk;
                last_clk_move = nw;
            } else if (P.aclock_valid && nw - last_clk_move > 2000000) {
                // Audio died on us (port error, thread gone). Re-anchor the
                // wall clock where the audio clock stopped so the picture
                // carries on from the same place instead of jumping.
                l_warn("video: audio clock stalled for 2s -- pacing on the wall clock from here");
                P.wall_base_us = nw - clk;
                __sync_synchronize();
                P.aclock_valid = false;
                last_clk_move = nw;
            }

            pthread_mutex_lock(&P.ring_lock);
            int count = P.count;
            int head = P.head;
            pthread_mutex_unlock(&P.ring_lock);

            if (count == 0) {
                if (P.decode_done && ring_count() == 0)
                    break;  // re-checked under the lock: nothing more is coming
                sceKernelDelayThread(1000);
                continue;
            }

            /*
             * Drop-to-latest. If the frame AFTER the head is due as well, the
             * head's slot has already passed and drawing it would only delay
             * the one the viewer should be looking at -- skip straight to the
             * newest due frame. Reading slots[head+1] unlocked is safe:
             * count >= 2 means that slot is filled and is not the one the
             * decode thread is writing into.
             */
            while (count >= 2) {
                int nxt = (head + 1) % P.nslots;
                if (P.slots[nxt].pts_us > clk)
                    break;
                pthread_mutex_lock(&P.ring_lock);
                P.head = nxt;
                P.count--;
                head = P.head;
                count = P.count;
                pthread_mutex_unlock(&P.ring_lock);
                P.dropped_late++;
            }

            FrameSlot *s = &P.slots[head];
            {
                // Fixed 30 fps tick (VIDEO_FRAME_PERIOD_US), like the
                // reference ports: never present two frames closer together
                // than one period, and never before the frame is due.
                // Sleeps in short hops so the skip button still answers
                // within a few ms.
                int64_t wait = s->pts_us - clk;
                if (last_present_us != 0) {
                    int64_t tick_wait = (last_present_us + P.frame_period_us) - now_us();
                    if (tick_wait > wait)
                        wait = tick_wait;
                }
                if (wait > 0) {
                    if (wait > 8000) wait = 8000;
                    sceKernelDelayThread((SceUInt) wait);
                    continue;
                }
            }

            draw_video_frame(s->rgb, s->w, s->h);
            P.presented++;
            last_present_us = now_us();

            pthread_mutex_lock(&P.ring_lock);
            P.head = (P.head + 1) % P.nslots;
            P.count--;
            pthread_mutex_unlock(&P.ring_lock);
        }

        // Let the audio tail finish rather than cutting it off mid-word. The
        // decode thread is throttled by the frame ring, so at natural EOF
        // only a few packets are still queued; the cap is just a backstop.
        if (!skipped && audioThreadUp) {
            int64_t deadline = now_us() + 1500000;
            while (!P.audio_done && now_us() < deadline)
                sceKernelDelayThread(2000);
        }

        int64_t play_end = now_us();
        double elapsed = (double) (play_end - play_start) / 1000000.0;
        double avg_fps = elapsed > 0.0 ? (double) P.presented / elapsed : 0.0;
        int div = P.presented > 0 ? P.presented : 1;
        l_info("video: loop exited! presented=%d, decoded=%d, dropped_late=%d, audio_frames_played=%u, "
               "elapsed=%.2fs, avg_fps=%.1f [decode=%.1fms/frame, yuv_convert=%.1fms/frame, "
               "tex_upload=%.1fms/frame, gl_draw+swap=%.1fms/frame]",
               P.presented, P.decoded, P.dropped_late, (unsigned) P.aplayed, elapsed, avg_fps,
               ((double) P.decode_us / 1000.0) / div,
               ((double) P.convert_us / 1000.0) / div,
               ((double) P.upload_us / 1000.0) / div,
               ((double) P.draw_us / 1000.0) / div);
    }

cleanup:
    // Order matters: stop the workers before tearing down anything they
    // touch (the audio port, the codec contexts, the format context).
    P.quit = true;
    __sync_synchronize();
    if (decodeThreadUp)
        pthread_join(decodeThread, NULL);
    if (audioThreadUp)
        pthread_join(audioThread, NULL);

    aq_drain_and_free();

    if (P.aport >= 0) {
        sceAudioOutReleasePort(P.aport);
        P.aport = -1;
    }
    if (P.swr) swr_free(&P.swr);
    // avcodec_free_context() joins FFmpeg's frame threads, so it runs here on
    // the same thread that created them in avcodec_open2().
    if (P.vctx) avcodec_free_context(&P.vctx);
    if (P.actx) avcodec_free_context(&P.actx);
    mp4_close(&P.mp4);
    // The ring's RGB565 buffers are 750-800KB each; don't hold ~2.3MB of
    // them for the rest of the session just because a cutscene played.
    free_frame_slots();
    P.presenting = false;

    l_success("video: %s (%s)", skipped ? "skipped" : "finished", path);
}
