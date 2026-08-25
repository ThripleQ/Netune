#include "ffmpeg_stream.h"
#include "infra/log.h"
#include "compat/utf8.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/dict.h>
#include <libswresample/swresample.h>

struct FFStream {
    AVFormatContext *fmt;
    AVCodecContext  *codec;
    SwrContext      *swr;
    AVPacket        *pkt;
    AVFrame         *frm;
    int stream_idx, channels, sample_rate;
    int eof, flushing;
    /* ── recorder: tee raw stream bytes to a cache file ──
       rec_pb is the custom AVIO handed to avformat (CUSTOM_IO, so we own
       its lifetime). tee_read forwards from rec_inner and mirrors the
       bytes into rec_file. In partial-continuation mode the cached prefix
       (rec_part_read) is served first, then rec_inner is resumed from the
       byte after the prefix and its bytes are appended back to the file. */
    AVIOContext *rec_pb;     /* our tee AVIO (managed here) */
    AVIOContext *rec_inner;  /* underlying source (avio_open of the url) */
    FILE        *rec_file;   /* write handle (.part "wb" / cache file "ab") */
    char        *rec_path;   /* .part path — removed unless committed */
    int          rec_committed;
    /* partial-continuation mode */
    FILE        *rec_part_read;  /* prefix read handle (partial mode) */
    int64_t      rec_part_size;  /* prefix byte count */
    int64_t      rec_part_pos;   /* prefix bytes served so far */
    int          rec_partial;    /* 1 = continuing from a partial cache */
    int          rec_probing;    /* 1 = avformat probe still running */
    /* single-chunk prefetch: pulled right after the probe so the partial
       prefix → network handover is served from memory, not a cold read */
    uint8_t     *rec_prefetch;
    size_t       rec_prefetch_cap, rec_prefetch_len, rec_prefetch_pos;
    int64_t      rec_pos;        /* logical stream position (for SEEK_CUR) */
    /* growing-file mode: a local file being appended by a background
       downloader. The file is read-only here; reads block (via grow_wait)
       when the file is exhausted until more data arrives or the stream
       finishes, so FFmpeg never sees a spurious EOF mid-download. */
    FILE        *grow_file;
    char        *grow_path;
    ffstream_wait_fn grow_wait;
    void        *grow_opaque;
    int          growing;
};

static void prefetch_clear(FFStream *s);   /* defined below, used by tee_seek */

/* Route FFmpeg's error diagnostics into Netune's log (stderr is captured
   by the TUI and unreadable). Only ERROR/WARNING are forwarded. */
static void ffmpeg_log_cb(void *avcl, int level, const char *fmt, va_list vl) {
    (void)avcl;
    if (level > AV_LOG_WARNING) return;
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, vl);
    LOG_ERROR("libav: %s", msg);
}

/* ── Recorder (raw-stream tee) ─────────────────────── */
static int tee_read(void *opaque, uint8_t *buf, int buf_size) {
    FFStream *s = (FFStream*)opaque;
    /* partial mode: serve the cached prefix from disk first */
    if (s->rec_partial && s->rec_part_read) {
        int64_t left = s->rec_part_size - s->rec_part_pos;
        if (left > 0) {
            size_t want = (size_t)buf_size;
            if ((int64_t)want > left) want = (size_t)left;
            size_t n = fread(buf, 1, want, s->rec_part_read);
            if (n > 0) {
                s->rec_part_pos += (int64_t)n;
                s->rec_pos += (int64_t)n;
                return (int)n;
            }
            if (ferror(s->rec_part_read)) return AVERROR(EIO);
        }
        /* prefix exhausted → resume from the network source. During the
           probe the prefix handle is kept open (the stream is rewound right
           after) and probed network bytes are NOT written to the file —
           otherwise a short prefix would leave a hole in the cache. */
        if (s->rec_probing) {
            int n = avio_read(s->rec_inner, buf, buf_size);
            if (n > 0) s->rec_pos += n;
            return n;
        }
        fclose(s->rec_part_read);
        s->rec_part_read = NULL;
    }
    /* serve from the prefetch chunk first (seamless handover) */
    if (s->rec_prefetch && s->rec_prefetch_pos < s->rec_prefetch_len) {
        size_t left = s->rec_prefetch_len - s->rec_prefetch_pos;
        size_t want = (size_t)buf_size;
        if (want > left) want = left;
        memcpy(buf, s->rec_prefetch + s->rec_prefetch_pos, want);
        s->rec_prefetch_pos += want;
        s->rec_pos += (int64_t)want;
        if (s->rec_file)
            fwrite(buf, 1, want, s->rec_file);
        return (int)want;
    }
    if (s->rec_prefetch) {   /* exhausted → drop it, read directly from now on */
        av_free(s->rec_prefetch);
        s->rec_prefetch = NULL;
        s->rec_prefetch_len = s->rec_prefetch_pos = 0;
    }
    int n = avio_read(s->rec_inner, buf, buf_size);
    if (n > 0) {
        s->rec_pos += n;
        if (s->rec_file)
            fwrite(buf, 1, (size_t)n, s->rec_file);
    }
    return n;
}

static int64_t tee_seek(void *opaque, int64_t offset, int whence) {
    FFStream *s = (FFStream*)opaque;
    if (!s->rec_partial)
        return avio_seek(s->rec_inner, offset, whence);  /* plain mode */
    if (whence == AVSEEK_SIZE) return -1;  /* total length unknown */
    if (whence == SEEK_END)    return -1;

    int64_t target = offset;
    if (whence == SEEK_CUR) target += s->rec_pos;

    /* seek within the cached prefix → serve from disk (no network).
       The prefix handle may already be closed (the prefix was exhausted
       during playback) — re-open it so seeks back into the cached region
       keep working instead of falling through to a negative network seek. */
    if (target <= s->rec_part_size) {
        if (!s->rec_part_read) {
            s->rec_part_read = fopen_utf8(s->rec_path, "rb");
            if (!s->rec_part_read) return -1;
        }
        if (fseek(s->rec_part_read, (long)target, SEEK_SET) != 0)
            return -1;
        s->rec_part_pos = target;
        s->rec_pos = target;
        /* keep the prefetch chunk: it holds the bytes right after the
           prefix, so it still lines up once playback reaches the prefix
           end — and rec_inner has already advanced past it, so dropping
           it here would orphan those bytes. */
        return target;
    }
    /* beyond the prefix → delegate to the source. A seek past the prefix
       means the fetched bytes are no longer contiguous with it, so the
       cache is frozen (rec_file closed, prefetch dropped) FIRST — even if
       the source seek fails below, the cache file is never corrupted by
       misaligned backfill. */
    prefetch_clear(s);
    if (!s->rec_probing) {
        /* probing keeps both handles alive so the stream can be rewound
           cleanly after the probe completes */
        if (s->rec_file)       { fclose(s->rec_file);       s->rec_file = NULL; }
        if (s->rec_part_read)  { fclose(s->rec_part_read);  s->rec_part_read = NULL; }
    }
    int64_t r = avio_seek(s->rec_inner, target - s->rec_part_size, SEEK_SET);
    if (r < 0) return r;
    s->rec_pos = target;
    return target;
}

/* ── Single-chunk prefetch (seamless partial → network handover) ──
   A chunk of the network stream is pulled right after the probe completes,
   so when the cached prefix runs out the handover is served from memory
   instead of a cold network read (one RTT + server response). The chunk is
   NOT the whole song — after it is consumed, reads fall back to the normal
   on-demand path. Consumed bytes are appended to the cache file so the
   file stays byte-contiguous. Any seek discards the chunk. */
#define PREFETCH_BYTES (512 * 1024)

static void prefetch_clear(FFStream *s) {
    if (s->rec_prefetch) {
        /* flush any unconsumed prefetch bytes to the cache file first.
           prefetch_start has already advanced rec_inner past these bytes,
           so dropping them without writing would leave a hole in the cache
           file: the next network read would resume at rec_part_size +
           PREFETCH_BYTES while the file only covers the prefix. */
        if (s->rec_file && s->rec_prefetch_pos < s->rec_prefetch_len) {
            size_t left = s->rec_prefetch_len - s->rec_prefetch_pos;
            fwrite(s->rec_prefetch + s->rec_prefetch_pos, 1, left, s->rec_file);
        }
        av_free(s->rec_prefetch);
        s->rec_prefetch = NULL;
        s->rec_prefetch_len = s->rec_prefetch_pos = 0;
    }
}

static void prefetch_start(FFStream *s) {
    prefetch_clear(s);
    s->rec_prefetch_cap = PREFETCH_BYTES;
    s->rec_prefetch = (uint8_t*)av_malloc(s->rec_prefetch_cap);
    if (!s->rec_prefetch) return;   /* no prefetch → cold handover, still works */
    size_t len = 0;
    while (len < s->rec_prefetch_cap) {
        int n = avio_read(s->rec_inner, s->rec_prefetch + len,
                          (int)(s->rec_prefetch_cap - len));
        if (n <= 0) break;   /* EOF / error: keep whatever we already have */
        len += (size_t)n;
    }
    s->rec_prefetch_len = len;
    s->rec_prefetch_pos = 0;
    if (len == 0) prefetch_clear(s);
}

static void recorder_close_io(FFStream *s) {
    if (s->rec_inner) { avio_close(s->rec_inner); s->rec_inner = NULL; }
    if (s->rec_pb)    { avio_context_free(&s->rec_pb); s->rec_pb = NULL; }
}

/* Stop recording. Unless already committed, the .part file is deleted
   (a partial-continuation file is kept — it is the cache entry). */
static void recorder_discard(FFStream *s) {
    if (s->rec_file)       { fclose(s->rec_file);       s->rec_file = NULL; }
    if (s->rec_part_read)  { fclose(s->rec_part_read);  s->rec_part_read = NULL; }
    prefetch_clear(s);
    if (s->rec_path) {
        if (!s->rec_committed) remove_utf8(s->rec_path);
        free(s->rec_path);
        s->rec_path = NULL;
    }
}

/* ── Shared decoder setup (codec + swresample + pkt/frm) ── */
static int setup_decoder(FFStream *s, int *sr, int *ch, int *dur) {
    if (avformat_find_stream_info(s->fmt, NULL) < 0) return -1;

    /* av_find_best_stream changed the decoder pointer type from
       AVCodec** (FFmpeg 4.x) to const AVCodec** (FFmpeg 5.x+). */
#if LIBAVCODEC_VERSION_MAJOR >= 59
    const AVCodec *codec = NULL;
    s->stream_idx = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
#else
    AVCodec *codec = NULL;
    s->stream_idx = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
#endif
    if (s->stream_idx < 0) return -1;

    AVCodecParameters *par = s->fmt->streams[s->stream_idx]->codecpar;
    s->codec = avcodec_alloc_context3(codec);
    if (!s->codec) return -1;
    if (avcodec_parameters_to_context(s->codec, par) < 0) return -1;
    if (avcodec_open2(s->codec, codec, NULL) < 0) return -1;

    s->sample_rate = s->codec->sample_rate;
#if LIBAVCODEC_VERSION_MAJOR >= 59
    s->channels = s->codec->ch_layout.nb_channels;
#else
    s->channels = s->codec->channels;
#endif

#if LIBAVCODEC_VERSION_MAJOR >= 59
    AVChannelLayout out_l = s->codec->ch_layout;
    swr_alloc_set_opts2(&s->swr,
        &out_l, AV_SAMPLE_FMT_S16, s->sample_rate,
        &s->codec->ch_layout, s->codec->sample_fmt, s->sample_rate, 0, NULL);
#else
    s->swr = swr_alloc_set_opts(NULL,
        s->codec->channel_layout, AV_SAMPLE_FMT_S16, s->sample_rate,
        s->codec->channel_layout, s->codec->sample_fmt, s->sample_rate,
        0, NULL);
#endif
    if (!s->swr || swr_init(s->swr) < 0) return -1;

    s->pkt = av_packet_alloc();
    s->frm = av_frame_alloc();
    if (!s->pkt || !s->frm) return -1;

    if (sr)  *sr  = s->sample_rate;
    if (ch)  *ch  = s->channels;
    if (dur) *dur = (int)(s->fmt->duration / AV_TIME_BASE);
    return 0;
}

FFStream* ffstream_open(const char *url, int *sr, int *ch, int *dur) {
    av_log_set_level(AV_LOG_ERROR);
    FFStream *s = calloc(1, sizeof(FFStream));
    if (!s) return NULL;

    if (avformat_open_input(&s->fmt, url, NULL, NULL) < 0) goto fail;
    s->fmt->max_analyze_duration = 10 * AV_TIME_BASE;
    if (setup_decoder(s, sr, ch, dur) < 0) goto fail;
    return s;

fail:
    ffstream_close(s);
    return NULL;
}

FFStream* ffstream_open_rec(const char *url, const char *rec_path,
                            int *sr, int *ch, int *dur) {
    av_log_set_level(AV_LOG_ERROR);
    FFStream *s = calloc(1, sizeof(FFStream));
    if (!s) return NULL;

    /* underlying source using FFmpeg's own protocols (http/https/redirects/
       range requests) — tee_read just forwards + mirrors the bytes */
    if (avio_open(&s->rec_inner, url, AVIO_FLAG_READ) < 0) goto fail;

    uint8_t *buf = (uint8_t*)av_malloc(64 * 1024);
    if (!buf) goto fail;
    s->rec_pb = avio_alloc_context(buf, 64 * 1024, 0, s,
                                   tee_read, NULL, tee_seek);
    if (!s->rec_pb) { av_free(buf); goto fail; }

    s->fmt = avformat_alloc_context();
    if (!s->fmt) goto fail;
    s->fmt->pb = s->rec_pb;
    s->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;   /* we manage rec_pb's lifetime */
    s->fmt->max_analyze_duration = 10 * AV_TIME_BASE;
    if (avformat_open_input(&s->fmt, url, NULL, NULL) < 0) goto fail;
    if (setup_decoder(s, sr, ch, dur) < 0) goto fail;

    /* Probe done — rewind the stream to its very start before recording.
       FFmpeg only rewinds itself for mpeg/mpegts; for other formats (e.g.
       WAV, whose probe consumes a large chunk) the position is left where
       probing stopped, which would truncate the cache head. If we cannot
       rewind, skip caching rather than write a truncated cache file. */
    int do_rec = rec_path && rec_path[0];
    if (do_rec && avio_seek(s->rec_pb, 0, SEEK_SET) < 0)
        do_rec = 0;

    /* open the recorder only after probe completes + rewind, so probe-time
       reads/seek never corrupt the .part — it always starts at the first
       stream byte */
    if (do_rec) {
        s->rec_path = strdup(rec_path);
        s->rec_file = fopen_utf8(rec_path, "wb");
        if (!s->rec_file) {   /* recorder unavailable → plain stream */
            free(s->rec_path);
            s->rec_path = NULL;
        }
    }
    return s;

fail:
    ffstream_close(s);
    return NULL;
}

FFStream* ffstream_open_partial(const char *url, const char *prefix_path,
                                int *sr, int *ch, int *dur) {
    av_log_set_level(AV_LOG_ERROR);
    av_log_set_callback(ffmpeg_log_cb);
    FFStream *s = calloc(1, sizeof(FFStream));
    if (!s) return NULL;

    /* cached prefix = the file's current size */
    struct stat st;
    if (stat_utf8(prefix_path, &st) != 0 || st.st_size <= 0) goto fail;
    s->rec_part_size = (int64_t)st.st_size;

    /* resume the network source from the byte after the prefix.
       The `offset` option is REQUIRED: without it FFmpeg's http protocol
       sees the server's Content-Range start (rec_part_size) and fails the
       internal check ("Unexpected offset: expected 0, got N"). */
    char range[64];
    snprintf(range, sizeof(range), "Range: bytes=%lld-\r\n",
             (long long)s->rec_part_size);
    char offbuf[32];
    snprintf(offbuf, sizeof(offbuf), "%lld", (long long)s->rec_part_size);
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "headers", range, 0);
    av_dict_set(&opts, "offset", offbuf, 0);
    int oret = avio_open2(&s->rec_inner, url, AVIO_FLAG_READ, NULL, &opts);
    av_dict_free(&opts);
    if (oret < 0) goto fail;

    uint8_t *buf = (uint8_t*)av_malloc(64 * 1024);
    if (!buf) goto fail;
    s->rec_pb = avio_alloc_context(buf, 64 * 1024, 0, s,
                                   tee_read, NULL, tee_seek);
    if (!s->rec_pb) { av_free(buf); goto fail; }

    /* partial mode init BEFORE probing — the probe reads the cached prefix
       through the AVIO; the append handle is opened only after probe
       completes so probe-time seeks never corrupt the file */
    s->rec_partial = 1;
    s->rec_probing = 1;   /* probe reads may cross the prefix — keep the
                             prefix handle alive and don't write the file yet */
    s->rec_part_pos = 0;
    s->rec_pos = 0;
    s->rec_path = strdup(prefix_path);   /* file already final: never unlink */
    s->rec_committed = 1;
    s->rec_part_read = fopen_utf8(prefix_path, "rb");

    s->fmt = avformat_alloc_context();
    if (!s->fmt) goto fail;
    s->fmt->pb = s->rec_pb;
    s->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    s->fmt->max_analyze_duration = 10 * AV_TIME_BASE;
    if (avformat_open_input(&s->fmt, url, NULL, NULL) < 0) goto fail;
    if (setup_decoder(s, sr, ch, dur) < 0) goto fail;

    /* Probe done. If the probe read past the prefix it consumed bytes from
       the network source and the tee AVIO buffer; rewind both so playback
       starts contiguous from byte 0 and backfill resumes exactly at the
       prefix boundary (Range start). The probed network bytes are discarded
       instead of leaving a hole in the cache file. */
    avio_close(s->rec_inner);
    {
        char range[64];
        snprintf(range, sizeof(range), "Range: bytes=%lld-\r\n",
                 (long long)s->rec_part_size);
        char offbuf[32];
        snprintf(offbuf, sizeof(offbuf), "%lld", (long long)s->rec_part_size);
        AVDictionary *opts = NULL;
        av_dict_set(&opts, "headers", range, 0);
        av_dict_set(&opts, "offset", offbuf, 0);
        int r = avio_open2(&s->rec_inner, url, AVIO_FLAG_READ, NULL, &opts);
        av_dict_free(&opts);
        if (r < 0) goto fail;
    }
    if (avio_seek(s->rec_pb, 0, SEEK_SET) < 0) goto fail;
    s->rec_probing = 0;
    s->rec_part_pos = 0;
    s->rec_pos = 0;

    /* open the append handle to backfill the rest of the file */
    if (s->rec_part_read) {
        s->rec_file = fopen_utf8(prefix_path, "ab");
        if (!s->rec_file) {   /* append unavailable → prefix-only play */
            fclose(s->rec_part_read);
            s->rec_part_read = NULL;
        }
    }
    /* pull one prefetch chunk now → the prefix→network handover is seamless */
    prefetch_start(s);
    return s;

fail:
    ffstream_close(s);
    return NULL;
}

/* ── Growing-file mode (background downloader) ─────────
   The local file is appended to by a downloader thread. Reads block at
   the file tail (via grow_wait) until more data arrives or the stream
   finishes, so FFmpeg never sees a spurious EOF mid-download. Seeks are
   served from the file directly (local random access). */
static int growing_read(void *opaque, uint8_t *buf, int buf_size) {
    FFStream *s = (FFStream*)opaque;
    for (;;) {
        size_t n = fread(buf, 1, (size_t)buf_size, s->grow_file);
        if (n > 0) return (int)n;
        if (ferror(s->grow_file)) return AVERROR(EIO);
        /* EOF — wait for the file to grow past the current read position
           (background downloader) */
        if (s->growing && s->grow_wait) {
            long pos = ftell(s->grow_file);
            if (pos < 0) return AVERROR(EIO);
            int w = s->grow_wait(s->grow_opaque, (int64_t)pos);
            if (w > 0) {
                /* stdio caches the EOF flag after a 0-byte fread; clear it
                   so the next fread re-reads the (now grown) file */
                clearerr(s->grow_file);
                continue;
            }
            if (w < 0) return AVERROR(EIO); /* error */
            /* finished — drain any bytes written between the EOF fread and
               the completion signal before declaring EOF, so the final
               chunk is never lost */
            clearerr(s->grow_file);
            n = fread(buf, 1, (size_t)buf_size, s->grow_file);
            if (n > 0) return (int)n;
            return AVERROR_EOF;
        }
        /* static file / no wait callback — plain EOF. Must return
           AVERROR_EOF (negative), NOT 0: FFmpeg's avio_read treats a 0
           return as "no data, retry" and loops forever in its bypass path. */
        return AVERROR_EOF;
    }
}

static int64_t growing_seek(void *opaque, int64_t offset, int whence) {
    FFStream *s = (FFStream*)opaque;
    if (whence == AVSEEK_SIZE) {
        struct stat st;
        if (stat_utf8(s->grow_path, &st) == 0) return st.st_size;
        return -1;
    }
    if (whence == SEEK_CUR) {
        long pos = ftell(s->grow_file);
        if (pos < 0) return -1;
        offset += pos;
    } else if (whence == SEEK_END) {
        /* the file is still growing — SEEK_END is unreliable, use the
           current on-disk size so seeks land on the watermark */
        struct stat st;
        if (stat_utf8(s->grow_path, &st) != 0) return -1;
        offset += st.st_size;
    }
    /* clamp to the current on-disk size: a seek past the watermark would
       put the read position in a file hole (beyond EOF) and the read side
       would have to wait for the downloader to cover it. Landing on the
       watermark keeps playback moving as the file grows — the downloader
       always writes from 0, so data past the current watermark does not
       exist yet. Also guard against negative offsets. */
    if (offset < 0) offset = 0;
    struct stat st;
    if (stat_utf8(s->grow_path, &st) == 0 && offset > st.st_size)
        offset = st.st_size;
    if (fseek(s->grow_file, (long)offset, SEEK_SET) != 0) return -1;
    return offset;
}

FFStream* ffstream_open_growing(const char *path, ffstream_wait_fn wait,
                                void *wait_opaque,
                                int *sr, int *ch, int *dur) {
    av_log_set_level(AV_LOG_ERROR);
    FFStream *s = calloc(1, sizeof(FFStream));
    if (!s) return NULL;

    s->grow_file = fopen_utf8(path, "rb");
    if (!s->grow_file) goto fail;
    /* unbuffered: the file is appended to by another thread, so reads must
       see freshly written bytes immediately (no stdio buffering) */
    setvbuf(s->grow_file, NULL, _IONBF, 0);
    s->grow_path = strdup(path);
    if (!s->grow_path) goto fail;
    s->grow_wait = wait;
    s->grow_opaque = wait_opaque;
    s->growing = 1;

    uint8_t *buf = (uint8_t*)av_malloc(64 * 1024);
    if (!buf) goto fail;
    s->rec_pb = avio_alloc_context(buf, 64 * 1024, 0, s,
                                   growing_read, NULL, growing_seek);
    if (!s->rec_pb) { av_free(buf); goto fail; }

    s->fmt = avformat_alloc_context();
    if (!s->fmt) goto fail;
    s->fmt->pb = s->rec_pb;
    s->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;   /* we manage rec_pb's lifetime */
    s->fmt->max_analyze_duration = 10 * AV_TIME_BASE;
    if (avformat_open_input(&s->fmt, path, NULL, NULL) < 0) goto fail;
    if (setup_decoder(s, sr, ch, dur) < 0) goto fail;
    return s;

fail:
    ffstream_close(s);
    return NULL;
}

int ffstream_decode(FFStream *s, int16_t *pcm, int max_frames) {
    if (!s || s->eof) return 0;

    while (1) {
        int ret = avcodec_receive_frame(s->codec, s->frm);
        if (ret == 0) {
            /* got a frame — resample to S16 interleaved */
            int out_samples = s->frm->nb_samples;
            if (out_samples > max_frames) out_samples = max_frames;
            uint8_t *out[1] = {(uint8_t*)pcm};
            int n = swr_convert(s->swr, out, out_samples,
                                (const uint8_t**)s->frm->data, s->frm->nb_samples);
            av_frame_unref(s->frm);
            return n > 0 ? n : 0;
        }
        if (ret == AVERROR(EAGAIN)) {
            /* need more input — read a packet */
            if (s->flushing) { s->eof = 1; return 0; }
            int r = av_read_frame(s->fmt, s->pkt);
            if (r < 0) {
                avcodec_send_packet(s->codec, NULL);
                s->flushing = 1;
                continue;
            }
            if (s->pkt->stream_index == s->stream_idx)
                avcodec_send_packet(s->codec, s->pkt);
            av_packet_unref(s->pkt);
            continue;
        }
        /* EOF or error */
        s->eof = 1; return 0;
    }
}

int ffstream_seek(FFStream *s, int64_t timestamp_sec) {
    if (!s) return -1;
    /* plain network recording: a seek breaks byte-continuity, so the .part
       is discarded. Partial-continuation mode keeps the cached prefix and
       lets tee_seek decide: seeks inside the prefix are served from disk
       (backfill stays contiguous), seeks past it freeze the file. Growing
       mode seeks are plain local-file seeks (clamped to the watermark in
       growing_seek) and must not touch the recorder fields. */
    if (!s->rec_partial && !s->growing)
        recorder_discard(s);
    int64_t ts = timestamp_sec * AV_TIME_BASE;
    if (av_seek_frame(s->fmt, -1, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return -1;
    avcodec_flush_buffers(s->codec);
    swr_close(s->swr);
    swr_init(s->swr);
    s->eof = 0;
    s->flushing = 0;
    return 0;
}

int ffstream_recording(const FFStream *s) {
    return s && s->rec_file;
}

int ffstream_recorder_commit(FFStream *s, const char *final_path) {
    if (!s || !s->rec_file || !s->rec_path || !final_path) return -1;
    fflush(s->rec_file);
    fclose(s->rec_file);
    s->rec_file = NULL;
    if (s->rec_part_read) { fclose(s->rec_part_read); s->rec_part_read = NULL; }
    if (strcmp(s->rec_path, final_path) == 0) {
        /* partial continuation: the file is already at its final location */
        s->rec_committed = 1;
        free(s->rec_path);
        s->rec_path = NULL;
        return 0;
    }
    if (rename_utf8(s->rec_path, final_path) != 0) {
        remove_utf8(s->rec_path);
        free(s->rec_path);
        s->rec_path = NULL;
        return -1;
    }
    s->rec_committed = 1;
    free(s->rec_path);
    s->rec_path = NULL;
    return 0;
}

const char *ffstream_media_ext(const FFStream *s) {
    if (!s || !s->fmt || !s->fmt->iformat || !s->fmt->iformat->name)
        return ".mp3";
    const char *n = s->fmt->iformat->name;
    if (strstr(n, "flac")) return ".flac";
    if (strstr(n, "wav") || strstr(n, "pcm")) return ".wav";
    if (strstr(n, "mp4") || strstr(n, "mov") || strstr(n, "m4a") ||
        strstr(n, "aac") || strstr(n, "adts")) return ".m4a";
    return ".mp3";
}

int ffstream_growing(const FFStream *s) {
    return s && s->growing;
}

int ffstream_growing_avail_sec(const FFStream *s, long bitrate) {
    if (!s || !s->growing || !s->grow_path) return -1;
    if (bitrate <= 0) return -1;
    struct stat st;
    if (stat_utf8(s->grow_path, &st) != 0 || st.st_size <= 0) return -1;
    return (int)((int64_t)st.st_size * 8 / bitrate);
}

long ffstream_bitrate(const FFStream *s) {
    if (!s || !s->fmt) return 0;
    /* Prefer the decoder context bitrate: it comes from the codec
       parameters (accurate, e.g. 320000 for a 320k mp3). fmt->bit_rate is
       estimated from the bytes read during probe — for a growing file that
       prefix is tiny, so it is wildly wrong (e.g. ~3.2k). */
    if (s->codec && s->codec->bit_rate > 0) return s->codec->bit_rate;
    return s->fmt->bit_rate;
}

/* For a growing (still-downloading) file, return the byte position the
   decoder has consumed so far (the read cursor of the local grow file),
   so the caller can derive a MEASURED average bitrate (bytes / playtime)
   that does not depend on the probe-time estimate. Returns 0 when not in
   growing mode or the position is unknown. */
int64_t ffstream_growing_bytes_read(const FFStream *s) {
    if (!s || !s->growing || !s->grow_file) return 0;
    long pos = ftell(s->grow_file);
    return pos > 0 ? (int64_t)pos : 0;
}

void ffstream_close(FFStream *s) {
    if (!s) return;
    recorder_discard(s);   /* drop unfinished .part (no-op once committed) */
    av_frame_free(&s->frm);
    av_packet_free(&s->pkt);
    swr_free(&s->swr);
    avcodec_free_context(&s->codec);
    if (s->fmt) avformat_close_input(&s->fmt);
    recorder_close_io(s);
    if (s->grow_file) { fclose(s->grow_file); s->grow_file = NULL; }
    free(s->grow_path);
    free(s);
}
