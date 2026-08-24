#include "ffmpeg_stream.h"
#include "infra/log.h"
#include "compat/utf8.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
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
    int64_t      rec_pos;        /* logical stream position (for SEEK_CUR) */
};

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
        /* prefix exhausted → resume from the network source */
        fclose(s->rec_part_read);
        s->rec_part_read = NULL;
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

    /* seek within the cached prefix → serve from disk (no network) */
    if (s->rec_part_read && target <= s->rec_part_size) {
        if (fseek(s->rec_part_read, (long)target, SEEK_SET) != 0)
            return -1;
        s->rec_part_pos = target;
        s->rec_pos = target;
        return target;
    }
    /* beyond the prefix → delegate to the source. After a seek past the
       prefix the fetched bytes are no longer contiguous with it, so stop
       appending (freeze the file). */
    int64_t r = avio_seek(s->rec_inner, target - s->rec_part_size, SEEK_SET);
    if (r < 0) return r;
    if (s->rec_file)       { fclose(s->rec_file);       s->rec_file = NULL; }
    if (s->rec_part_read)  { fclose(s->rec_part_read);  s->rec_part_read = NULL; }
    s->rec_pos = target;
    return target;
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

    /* open the recorder only after probe completes, so probe-time seeks
       never corrupt the .part — it always starts at the first stream byte */
    if (rec_path && rec_path[0]) {
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
    FFStream *s = calloc(1, sizeof(FFStream));
    if (!s) return NULL;

    /* cached prefix = the file's current size */
    struct stat st;
    if (stat_utf8(prefix_path, &st) != 0 || st.st_size <= 0) goto fail;
    s->rec_part_size = (int64_t)st.st_size;

    /* resume the network source from the byte after the prefix */
    char range[64];
    snprintf(range, sizeof(range), "Range: bytes=%lld-\r\n",
             (long long)s->rec_part_size);
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "headers", range, 0);
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

    /* probe done → open the append handle to backfill the rest of the file */
    if (s->rec_part_read) {
        s->rec_file = fopen_utf8(prefix_path, "ab");
        if (!s->rec_file) {   /* append unavailable → prefix-only play */
            fclose(s->rec_part_read);
            s->rec_part_read = NULL;
        }
    }
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
       (backfill stays contiguous), seeks past it freeze the file. */
    if (!s->rec_partial)
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

void ffstream_close(FFStream *s) {
    if (!s) return;
    recorder_discard(s);   /* drop unfinished .part (no-op once committed) */
    av_frame_free(&s->frm);
    av_packet_free(&s->pkt);
    swr_free(&s->swr);
    avcodec_free_context(&s->codec);
    if (s->fmt) avformat_close_input(&s->fmt);
    recorder_close_io(s);
    free(s);
}
