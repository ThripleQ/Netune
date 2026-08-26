#include "playback_coordinator.h"
#include "core/decoder_manager.h"
#include "core/audio_output_mgr.h"
#include "core/music_source.h"
#include "core/music_source_manager.h"
#include "core/audio_cache.h"
#include "core/cache_segments.h"
#include "core/stream_downloader.h"
#include "plugins/decoders/ffmpeg/ffmpeg_stream.h"
#include "plugins/music_sources/netease/netease_quality.h"
#include "infra/config.h"
#include "core/event_bus.h"
#include "core/spectrum.h"
#include "infra/log.h"
#include "compat/utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>


/* ── Constants ──────────────────────────────────────── */
#define FRAMES_PER_CHUNK 4096
#define PROGRESS_INTERVAL_MS 16
/* Minimum prefix a background downloader must fetch before the playback
   thread opens the .part for FFmpeg probing (probe needs a few KB; 64KB
   covers ID3 + first frames comfortably). */
#define MIN_PREFIX_BYTES (64 * 1024)

/* ── Commands ───────────────────────────────────────── */
typedef enum {
    CMD_NONE,
    CMD_PLAY,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_STOP,
    CMD_SEEK,
    CMD_RELOAD,
    CMD_QUIT,
} CmdType;

typedef struct {
    CmdType type;
    char    path[1024];
    int     seek_frame;
} Command;

#define CMD_QUEUE_SIZE 32

typedef struct {
    Command items[CMD_QUEUE_SIZE];
    int     head, tail, count;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
} CmdQueue;

static void cmd_queue_init(CmdQueue *q) {
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->head = q->tail = q->count = 0;
}

static void cmd_queue_push(CmdQueue *q, const Command *cmd) {
    pthread_mutex_lock(&q->mutex);
    if (q->count < CMD_QUEUE_SIZE) {
        q->items[q->tail] = *cmd;
        q->tail = (q->tail + 1) % CMD_QUEUE_SIZE;
        q->count++;
        pthread_cond_signal(&q->cond);
    }
    pthread_mutex_unlock(&q->mutex);
}

/* Push a CMD_PLAY, discarding any older CMD_PLAY commands still queued.
   Rapid Next presses only ever switch to the newest target — without this
   the playback thread would process every stale play command one by one,
   each with a full close/open cycle, stalling audio. */
static void cmd_queue_push_play(CmdQueue *q, const Command *cmd) {
    pthread_mutex_lock(&q->mutex);
    /* drop stale CMD_PLAY commands already in the queue */
    for (int i = 0; i < q->count; ) {
        int idx = (q->head + i) % CMD_QUEUE_SIZE;
        if (q->items[idx].type == CMD_PLAY) {
            for (int j = i; j < q->count - 1; j++) {
                int a = (q->head + j) % CMD_QUEUE_SIZE;
                int b = (q->head + j + 1) % CMD_QUEUE_SIZE;
                q->items[a] = q->items[b];
            }
            q->tail = (q->tail + CMD_QUEUE_SIZE - 1) % CMD_QUEUE_SIZE;
            q->count--;
        } else {
            i++;
        }
    }
    if (q->count >= CMD_QUEUE_SIZE) {
        /* full of non-PLAY commands: evict the oldest to make room —
           losing a play is worse than losing any other command */
        q->head = (q->head + 1) % CMD_QUEUE_SIZE;
        q->count--;
    }
    if (q->count < CMD_QUEUE_SIZE) {
        q->items[q->tail] = *cmd;
        q->tail = (q->tail + 1) % CMD_QUEUE_SIZE;
        q->count++;
        pthread_cond_signal(&q->cond);
    }
    pthread_mutex_unlock(&q->mutex);
}

static Command cmd_queue_pop(CmdQueue *q) {
    Command cmd = {0};
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0)
        pthread_cond_wait(&q->cond, &q->mutex);
    cmd = q->items[q->head];
    q->head = (q->head + 1) % CMD_QUEUE_SIZE;
    q->count--;
    pthread_mutex_unlock(&q->mutex);
    return cmd;
}

/* ── Global state ───────────────────────────────────── */
static pthread_t       g_thread;
static CmdQueue        g_cmd_queue;
static volatile bool   g_running = false;

/* ── Active audio-cache context ───────────────────────
   Set by open_stream on the netease branch (which song/quality is being
   recorded) and consumed at natural end-of-stream to commit the .part.
   Only the playback thread touches these, so no locking is needed. */
static char g_rec_song[128]  = {0};
static char g_rec_level[32]  = {0};
static int  g_rec_active     = 0;
/* Background downloader for the current cache-miss stream (growing-file
   mode). g_downloader non-NULL means a download is in flight; g_part_path
   is the .part it writes, owned here. */
static StreamDownloader *g_downloader = NULL;
static char *g_part_path = NULL;
/* URL of the current netease stream — kept so do_seek can restart the
   downloader at a new byte offset with the same source. */
static char              g_stream_url[2048] = {0};
/* Holey-cache replay state (M5): the current stream re-fetches an existing
   cache file in place. g_segs holds the valid byte regions of that file
   (already on disk, sorted/non-overlapping — the "known good" parts);
   g_reuse_cache marks that g_part_path is already the final cache entry
   (committed in place, never renamed). */
static CacheSegList      g_segs;
static int               g_reuse_cache = 0;

/* ── Event bus handlers ────────────────────────────── */
static void on_app_shutdown(const BusEvent *ev, void *ud) {
    (void)ev; (void)ud;
    Command cmd = {.type = CMD_QUIT};
    cmd_queue_push(&g_cmd_queue, &cmd);
}

static void on_play_start(const BusEvent *ev, void *ud) {
    (void)ud;
    const char *path = (const char*)ev->data;
    Command cmd = {.type = CMD_PLAY};
    if (path) snprintf(cmd.path, sizeof(cmd.path), "%s", path);
    cmd_queue_push_play(&g_cmd_queue, &cmd);
}

static void on_play_pause(const BusEvent *ev, void *ud) {
    (void)ev; (void)ud;
    Command cmd = {.type = CMD_PAUSE};
    cmd_queue_push(&g_cmd_queue, &cmd);
}

static void on_play_resume(const BusEvent *ev, void *ud) {
    (void)ev; (void)ud;
    Command cmd = {.type = CMD_RESUME};
    cmd_queue_push(&g_cmd_queue, &cmd);
}

static void on_play_stop(const BusEvent *ev, void *ud) {
    (void)ev; (void)ud;
    Command cmd = {.type = CMD_STOP};
    cmd_queue_push(&g_cmd_queue, &cmd);
}

static void on_seek(const BusEvent *ev, void *ud) {
    (void)ud;
    int sec = ev->data ? *(int*)ev->data : 0;
    Command cmd = {.type = CMD_SEEK, .seek_frame = sec};
    cmd_queue_push(&g_cmd_queue, &cmd);
}

static void on_play_reload(const BusEvent *ev, void *ud) {
    (void)ud;
    const PlaybackReloadCmd *r = (const PlaybackReloadCmd*)ev->data;
    Command cmd = {.type = CMD_RELOAD};
    if (r) {
        snprintf(cmd.path, sizeof(cmd.path), "%s", r->id);
        cmd.seek_frame = r->seek_sec;
    }
    cmd_queue_push(&g_cmd_queue, &cmd);
}

/* ── Playback thread ────────────────────────────────── */
typedef enum { PS_STOPPED, PS_PLAYING, PS_PAUSED } PlayState;

/* ── Try to pop a command without blocking. Returns false if empty. ── */
static bool cmd_queue_try_pop(CmdQueue *q, Command *out) {
    bool got = false;
    pthread_mutex_lock(&q->mutex);
    if (q->count > 0) {
        *out = q->items[q->head];
        q->head = (q->head + 1) % CMD_QUEUE_SIZE;
        q->count--;
        got = true;
    }
    pthread_mutex_unlock(&q->mutex);
    return got;
}

/* Tear down the audio output. The backend shutdown drains the hardware
   buffer (snd_pcm_drain / pa_simple_drain) so the tail of the current
   track plays out smoothly — a hard drop (snd_pcm_drop) clicks/pops.
   The drain is short (≤~100ms with the small ALSA buffer) and CMD_PLAY
   coalescing means rapid skipping only ever drains once. */
static void audio_teardown(AudioOutput *audio) {
    if (audio) audio_output_destroy(audio);
}

/* ── Shared stream-open (CMD_PLAY / CMD_RELOAD) ───────
   Tears down the previous stream, then opens `path` — a local file or a
   netease song id (URL re-resolved, so a quality switch picks up the new
   per-song override / global default). On success fills the thread's
   ffstream/decoder/audio + samplerate/channels/total_frames.
   Returns: 0 = ready, 1 = no play URL (caller should skip), -1 = failed. */

/* Called right before a stream is torn down at stop/switch (NOT at
   end-of-stream, which commits a complete cache): keep whatever contiguous
   bytes were received as a partial cache entry instead of discarding them,
   so a half-heard track still starts instantly next time. Handles both the
   growing-file mode (background downloader) and the legacy recorder mode
   (partial-continuation backfill). */
static void cache_keep_partial(FFStream *ffstream) {
    if (!ffstream || !g_rec_active) return;
    /* growing-file mode: stop the downloader and keep the .part as a
       partial cache entry (rename .part → final). If the downloader had
       already finished writing the whole file, mark it complete so the
       next play hits the local file directly instead of failing a
       byte-range continuation on an already-complete file. */
    if (g_downloader) {
        int done_clean = stream_downloader_done(g_downloader) &&
                         !stream_downloader_failed(g_downloader);
        int64_t keep = stream_downloader_watermark(g_downloader);
        int64_t seq_start = stream_downloader_start_byte(g_downloader);
        /* Build the segment list to persist: everything the sequential
           downloader wrote [seq_start, seq_start+keep), plus whatever
           valid regions were already on disk (g_segs, when replaying in
           place). */
        CacheSegList segs;
        cache_seglist_init(&segs);
        if (keep > 0) cache_seglist_add(&segs, seq_start, keep);
        if (g_reuse_cache) {
            for (int k = 0; k < g_segs.count; k++)
                cache_seglist_add(&segs, g_segs.segs[k].start,
                                  g_segs.segs[k].len);
        }
        int complete = done_clean;
        if (!complete) {
            /* if the retained regions plus the sequential download now form
               one contiguous run covering the whole file, it is complete */
            struct stat st;
            if (stat_utf8(g_part_path, &st) == 0 && st.st_size > 0 &&
                segs.count == 1 && segs.segs[0].start == 0 &&
                segs.segs[0].len >= (int64_t)st.st_size)
                complete = 1;
        }
        stream_downloader_destroy(g_downloader);
        g_downloader = NULL;
        if (g_part_path) {
            struct stat st;
            if (keep > 0 && stat_utf8(g_part_path, &st) == 0 &&
                st.st_size > 0) {
                if (g_reuse_cache) {
                    /* the file is already the final cache entry — commit in
                       place with the current byte regions */
                    audio_cache_commit(g_rec_song, g_part_path, g_rec_level,
                                       complete, &segs);
                } else {
                    char *final = audio_cache_final_path(
                        g_rec_song, ffstream_media_ext(ffstream));
                    if (final) {
                        if (rename_utf8(g_part_path, final) == 0)
                            audio_cache_commit(g_rec_song, final,
                                               g_rec_level, complete, &segs);
                        free(final);
                    }
                }
            } else {
                remove_utf8(g_part_path);
            }
            free(g_part_path);
            g_part_path = NULL;
        }
        cache_seglist_free(&segs);
        g_rec_active = 0;
        return;
    }
    /* legacy recorder mode (partial-continuation backfill) */
    if (!ffstream_recording(ffstream)) {  /* seek'd or append unavailable */
        g_rec_active = 0;
        return;
    }
    char *final = audio_cache_final_path(g_rec_song,
                                         ffstream_media_ext(ffstream));
    if (final) {
        if (ffstream_recorder_commit(ffstream, final) == 0) {
            CacheSegList empty;
            cache_seglist_init(&empty);
            audio_cache_commit(g_rec_song, final, g_rec_level, 0, &empty);
            cache_seglist_free(&empty);
        }
        free(final);
    }
    g_rec_active = 0;
}

/* Wait callback for ffstream_open_growing: blocks until the background
   downloader has written past the read position `pos`, finishes, or fails.
   Returns 1 = more data may be available (retry the read),
   0 = the download finished (EOF), -1 = error. */
static int wait_more_data(void *opaque, int64_t pos) {
    (void)opaque;
    StreamDownloader *dl = g_downloader;
    int64_t need = pos + 1;
    /* Retained regions of a holey cache are on disk and immediately
       available (no downloader involved / not yet overwritten) — check
       BEFORE the blocking wait, or playing the prefix would stall 500ms per
       read while the re-fetch lags behind. */
    if (cache_seglist_contains(&g_segs, pos)) return 1;
    /* The sequential downloader covers [seq_start, seq_start+watermark);
       requests are relative to its start (it may resume past a retained
       prefix). A position before seq_start that is not in a retained
       segment is a gap nothing will fill — fail rather than stall. */
    if (dl) {
        int64_t seq_start = stream_downloader_start_byte(dl);
        if (pos >= seq_start) {
            need = pos - seq_start + 1;
        } else {
            return -1;
        }
    }
    if (stream_downloader_failed(dl)) return -1;
    /* wait for the downloader to reach one byte past the read position;
       the 500ms cap keeps a stalled download from blocking the playback
       thread forever. A timeout here means "no progress yet" — the read is
       retried (the downloader either advances later or fails, and the wait
       loop below converges to EOF / error then). */
    stream_downloader_wait_watermark(dl, need, 500);
    if (stream_downloader_failed(dl)) return -1;
    if (stream_downloader_watermark(dl) >= need) return 1;  /* data available */
    if (stream_downloader_done(dl)) return 0;               /* finished past end */
    return 1;  /* retry the read (new data arrived, or still waiting) */
}

/* Fallback for a cache file whose replay downloader failed to start/probe:
   the file may actually be complete (downloaded to the end but marked
   partial because playback stopped before EOF), so try a plain local
   decode instead of failing the track. Returns 0 = ok (decoder set), -1 =
   the file is not playable locally either. */
static int open_cached_local(const char *cache_path, Decoder **decoder,
                             int *samplerate, int *channels,
                             int *total_frames) {
    *decoder = decoder_open(cache_path);
    if (!*decoder) return -1;
    DecoderInfo info;
    decoder_get_info(*decoder, &info);
    *samplerate   = info.sample_rate;
    *channels     = info.channels;
    *total_frames = info.total_frames;
    return 0;
}

static int open_stream(const char *path,
                       FFStream **ffstream, Decoder **decoder,
                       AudioOutput **audio,
                       int *samplerate, int *channels, int *total_frames) {
    if (*ffstream) {
        cache_keep_partial(*ffstream);   /* preserve partial recording */
        ffstream_close(*ffstream);
        *ffstream = NULL;
    }
    if (*decoder)  { decoder_close(*decoder); *decoder = NULL; }
    audio_teardown(*audio); *audio = NULL;
    /* fresh stream: no retained cache regions carried across */
    cache_seglist_free(&g_segs);   /* release the previous track's list */
    cache_seglist_init(&g_segs);
    g_reuse_cache = 0;

    /* Determine if path points to a local file (vs. a streaming
       source ID such as a netease song ID).  Checks, in order:
       1. file:// URL prefix
       2. POSIX absolute path (starts with '/')
       3. User home directory (starts with '~')
       4. Windows drive-letter absolute path (e.g. C:\ or C:/)
       5. Contains a path separator AND a dot — likely a
          relative file path with an extension */
    int is_local = 0;
    if (strncmp(path, "file://", 7) == 0) {
        is_local = 1;
    } else if (path[0] == '/' || path[0] == '~') {
        is_local = 1;
    } else if (((path[0] >= 'A' && path[0] <= 'Z') ||
                (path[0] >= 'a' && path[0] <= 'z')) &&
               path[1] == ':') {
        is_local = 1;
    } else if ((strchr(path, '/') || strchr(path, '\\')) &&
               strchr(path, '.')) {
        is_local = 1;
    }

    if (!is_local) {
        /* resolve the effective quality once (in-memory when cached) so we
           can (a) reuse a cached copy recorded at the same level and
           (b) tag the recording with the level it was cached at */
        g_rec_active = 0;
        if (g_downloader) {   /* safety: cache_keep_partial should have
                                 cleared this already */
            stream_downloader_destroy(g_downloader);
            g_downloader = NULL;
        }
        free(g_part_path);
        g_part_path = NULL;
        snprintf(g_rec_song, sizeof(g_rec_song), "%s", path);
        g_rec_level[0] = '\0';

        char *level = nq_resolve_level(path);
        if (!level) {
            LOG_WARN("No usable quality for %s", path);
            return 1;  /* unplayable — caller decides skip/error */
        }
        snprintf(g_rec_level, sizeof(g_rec_level), "%s", level);

        /* 1. cache hit → play the cached file directly (offline, the stream
              never touches the network when the copy is complete; a partial
              prefix continues from disk and resumes the rest via network) */
        char cache_path[1100];
        int  cache_complete = 1;
        CacheSegList cache_segs;
        cache_seglist_init(&cache_segs);
        if (audio_cache_find(path, level, cache_path, sizeof(cache_path),
                             &cache_complete, &cache_segs) == 0) {
            if (cache_complete) {
                *decoder = decoder_open(cache_path);
                if (!*decoder) {
                    /* no local decoder for this container (e.g. a cached
                       m4a/aac stream) — play the cached file via FFmpeg
                       instead of failing the whole track */
                    LOG_WARN("No local decoder for cached %s, using FFmpeg",
                             cache_path);
                    int dur_sec = 0;
                    *ffstream = ffstream_open(cache_path, samplerate,
                                              channels, &dur_sec);
                    if (!*ffstream) {
                        LOG_ERROR("Cannot open cached: %s", cache_path);
                        cache_seglist_free(&cache_segs);
                        free(level);
                        return -1;
                    }
                    *total_frames = (int64_t)dur_sec * (*samplerate);
                    audio_cache_touch(path);
                    cache_seglist_free(&cache_segs);
                } else {
                    DecoderInfo info;
                    decoder_get_info(*decoder, &info);
                    *samplerate   = info.sample_rate;
                    *channels     = info.channels;
                    *total_frames = info.total_frames;
                    audio_cache_touch(path);
                    cache_seglist_free(&cache_segs);
                }
            } else {
                /* partial cache (any shape: prefix only, prefix+tail, ...) →
                   replay it in place via the growing path: a no-truncate
                   downloader re-fetches the file from byte 0 while playback
                   reads the local file. Retained regions play instantly (the
                   segment list), and every gap fills as the download
                   advances — so seeks no longer discard the cache. */
                char url[2048] = {0};
                MusicSource *src = music_source_get("netease");
                if (!src || !src->get_play_url ||
                    src->get_play_url(path, 0, url, sizeof(url)) != 0 || !url[0]) {
                    LOG_WARN("No play URL for %s", path);
                    cache_seglist_free(&cache_segs);
                    free(level);
                    return 1;  /* unplayable — caller decides skip/error */
                }
                LOG_INFO("Continuing partial cache: %s", path);
                /* Greedy forward-fill: the sequential downloader resumes at
                   the end of the contiguous on-disk prefix (not byte 0), so
                   already-cached bytes are never re-fetched — it just runs
                   ahead filling every gap to EOF. */
                int64_t prefix_end = 0;
                if (cache_segs.count > 0 && cache_segs.segs[0].start == 0)
                    prefix_end = cache_segs.segs[0].len;
                StreamDownloader *dl = stream_downloader_create_resume(
                    url, cache_path, prefix_end);
                if (!dl || stream_downloader_start(dl) != 0) {
                    if (dl) stream_downloader_destroy(dl);
                    LOG_WARN("Partial cache replay: downloader failed %s",
                             path);
                    if (open_cached_local(cache_path, decoder, samplerate,
                                          channels, total_frames) == 0) {
                        audio_cache_touch(path);
                        cache_seglist_free(&cache_segs);
                        free(level);
                        return 0;
                    }
                    cache_seglist_free(&cache_segs);
                    free(level);
                    return 1;
                }
                int w = stream_downloader_wait_watermark(
                    dl, MIN_PREFIX_BYTES, 15000);
                if (w < 0 ||
                    (w == 0 && stream_downloader_watermark(dl) <= 0)) {
                    stream_downloader_destroy(dl);
                    LOG_WARN("Partial cache replay: prefix wait failed %s",
                             path);
                    if (open_cached_local(cache_path, decoder, samplerate,
                                          channels, total_frames) == 0) {
                        audio_cache_touch(path);
                        cache_seglist_free(&cache_segs);
                        free(level);
                        return 0;
                    }
                    cache_seglist_free(&cache_segs);
                    free(level);
                    return 1;
                }
                int dur_sec = 0;
                *ffstream = ffstream_open_growing(
                    cache_path, wait_more_data, dl,
                    samplerate, channels, &dur_sec);
                if (!*ffstream) {
                    stream_downloader_destroy(dl);
                    LOG_WARN("Partial cache replay: growing open failed %s",
                             path);
                    if (open_cached_local(cache_path, decoder, samplerate,
                                          channels, total_frames) == 0) {
                        audio_cache_touch(path);
                        cache_seglist_free(&cache_segs);
                        free(level);
                        return 0;
                    }
                    cache_seglist_free(&cache_segs);
                    free(level);
                    return -1;
                }
                *total_frames = (int64_t)dur_sec * (*samplerate);
                g_rec_active = 1;
                g_downloader = dl;
                g_part_path = strdup(cache_path);
                g_reuse_cache = 1;
                g_segs = cache_segs;   /* retained on-disk valid regions */
                cache_segs.segs = NULL;  /* ownership moved to g_segs */
                snprintf(g_stream_url, sizeof g_stream_url, "%s", url);
                ffstream_set_growing_total(
                    *ffstream, stream_downloader_total_size(dl));
                ffstream_set_growing_regions(
                    *ffstream, g_segs.segs, g_segs.count);
                audio_cache_touch(path);
                free(level);
                return 0;
            }
        } else {
            /* 2. cache miss → background downloader writes the .part while
                  the playback thread reads it locally (download & playback
                  decoupled: the download runs ahead independently) */
            char url[2048] = {0};
            MusicSource *src = music_source_get("netease");
            if (!src || !src->get_play_url ||
                src->get_play_url(path, 0, url, sizeof(url)) != 0 || !url[0]) {
                LOG_WARN("No play URL for %s", path);
                cache_seglist_free(&cache_segs);
                free(level);
                return 1;  /* unplayable — caller decides skip/error */
            }
            LOG_INFO("Streaming netease (download+play): %s", path);
            /* remember the source URL so do_seek can restart the downloader
               at the switch target; no downloader carries across streams */
            snprintf(g_stream_url, sizeof g_stream_url, "%s", url);
            char *part = audio_cache_part_path(path);
            if (part)
                audio_cache_ensure_dir();   /* .part lives in the cache dir */
            if (!part) {
                /* caching disabled → plain network stream (no downloader) */
                int dur_sec = 0;
                *ffstream = ffstream_open_rec(url, NULL, samplerate,
                                              channels, &dur_sec);
                if (!*ffstream) {
                    LOG_WARN("FFmpeg stream open failed %s", path);
                    cache_seglist_free(&cache_segs);
                    free(level);
                    return -1;
                }
                *total_frames = (int64_t)dur_sec * (*samplerate);
                g_rec_active = 0;
            } else {
                StreamDownloader *dl = stream_downloader_create(url, part);
                if (!dl) {
                    free(part);
                    cache_seglist_free(&cache_segs);
                    free(level);
                    return -1;
                }
                if (stream_downloader_start(dl) != 0) {
                    stream_downloader_destroy(dl);
                    free(part);
                    cache_seglist_free(&cache_segs);
                    free(level);
                    return -1;
                }
                /* wait for a usable prefix so FFmpeg's probe has data */
                int w = stream_downloader_wait_watermark(dl, MIN_PREFIX_BYTES,
                                                         15000);
                if (w < 0 ||
                    (w == 0 && stream_downloader_watermark(dl) <= 0)) {
                    /* failed, or timed out with zero bytes — remove any
                       partial .part the downloader may have written (it is
                       joined by destroy, so the file is no longer touched) */
                    LOG_WARN("Downloader prefix wait failed for %s", path);
                    stream_downloader_destroy(dl);
                    remove_utf8(part);
                    free(part);
                    cache_seglist_free(&cache_segs);
                    free(level);
                    return -1;
                }
                int dur_sec = 0;
                *ffstream = ffstream_open_growing(part, wait_more_data, dl,
                                                  samplerate, channels,
                                                  &dur_sec);
                if (!*ffstream) {
                    LOG_WARN("FFmpeg growing open failed %s", path);
                    stream_downloader_destroy(dl);
                    remove_utf8(part);   /* orphan .part: probe failed */
                    free(part);
                    cache_seglist_free(&cache_segs);
                    free(level);
                    return -1;
                }
                *total_frames = (int64_t)dur_sec * (*samplerate);
                g_rec_active = 1;
                g_downloader = dl;
                g_part_path = part;   /* ownership transferred */
                /* Once the Content-Length is in, declare the stream's true
                   size so FFmpeg reports the real duration + seek math and
                   the progress bar total is correct from the start. */
                ffstream_set_growing_total(*ffstream,
                                           stream_downloader_total_size(dl));
            }
            cache_seglist_free(&cache_segs);
        }
        free(level);
    } else {
        *decoder = decoder_open(path);
        if (!*decoder) {
            LOG_ERROR("Cannot open: %s", path);
            return -1;
        }
        DecoderInfo info;
        decoder_get_info(*decoder, &info);
        *samplerate   = info.sample_rate;
        *channels     = info.channels;
        *total_frames = info.total_frames;
    }

    *audio = audio_output_create(*samplerate, *channels);
    if (!*audio) {
        if (*ffstream) { ffstream_close(*ffstream); *ffstream = NULL; }
        if (*decoder)  { decoder_close(*decoder); *decoder = NULL; }
        return -1;
    }
    return 0;
}

/* Effective bitrate (bits/sec) for the current stream, computed ONCE per
   call site and shared by everything that needs it:
   - the progress event's bitrate tag (status bar),
   - the growing-file total-length correction,
   - the growing seek clamp.
   Declared bitrate (codec/container metadata) is preferred; for a growing
   stream with none, it is MEASURED from what has actually been consumed so
   far (bytes read / playtime), because the probe-time estimate on a tiny
   prefix is wildly wrong. Returns 0 when unknown. */
static long effective_bitrate(FFStream *ffstream, int64_t current_frame,
                              int samplerate) {
    if (!ffstream) return 0;
    long br = ffstream_bitrate(ffstream);
    if (br > 0) return br;
    if (!ffstream_growing(ffstream)) return 0;
    int64_t read_b = ffstream_growing_bytes_read(ffstream);
    int played_sec = samplerate > 0 ? (int)(current_frame / samplerate) : 0;
    if (read_b > 0 && played_sec > 0)
        return (long)(read_b * 8 / played_sec);
    return 0;
}

/* Contiguous bytes playable from position 0 right now. The sequential
   downloader covers [seq_start, seq_start+watermark); that run is part of
   the continuous prefix only when seq_start sits at (or inside) the
   retained [0, ...) segment. Holes beyond the continuous run are NOT
   counted — unlike the file size, which a parallel Range downloader
   inflates with holey regions, this is the true continuous-play bound. */
static int64_t growing_avail_bytes(void) {
    int64_t avail = 0;
    if (g_segs.count > 0 && g_segs.segs[0].start == 0)
        avail = g_segs.segs[0].len;
    if (g_downloader) {
        int64_t sb = stream_downloader_start_byte(g_downloader);
        int64_t wm = stream_downloader_watermark(g_downloader);
        if (sb <= avail && sb + wm > avail)
            avail = sb + wm;
    }
    return avail;
}

/* Apply a seek (seconds) to the current stream, updating current_frame. */
static void do_seek(FFStream *ffstream, Decoder *decoder, int samplerate,
                    int total_frames, int seek_sec, int *current_frame) {
    int target = seek_sec * samplerate;
    if (target < 0) target = 0;
    /* Growing-file mode is handled first: the total_frames probe estimate
       is tiny and useless as a bound — the real bound is the download
       watermark. To keep a quality switch continuous, wait (bounded) for
       the background downloader to cover the target byte, so the seek
       lands at the resume position instead of snapping back to the start.
       On a slow link the timeout just resumes at the current watermark. */
    if (ffstream && ffstream_growing(ffstream)) {
        long br = effective_bitrate(ffstream, *current_frame, samplerate);
        int64_t avail_bytes = growing_avail_bytes();
        int avail = (br > 0) ? (int)(avail_bytes * 8 / br) : 0;
        if (g_downloader && br > 0 && seek_sec > avail) {
            int64_t target_byte = (int64_t)seek_sec * br / 8;
            /* Seek target already on disk (a retained segment, e.g. the
               tail of a holey cache) → play it directly. */
            int target_in_segs = cache_seglist_contains(&g_segs, target_byte);
            /* Target inside the sequential downloader's covered run → the
               downloader keeps running ahead from where it is; just seek. */
            int64_t seq_start = stream_downloader_start_byte(g_downloader);
            int64_t seq_end = seq_start +
                              stream_downloader_watermark(g_downloader);
            int target_in_seq = (target_byte >= seq_start &&
                                 target_byte < seq_end);
            if (!target_in_segs && !target_in_seq) {
                /* Seek mode: abandon the current forward-fill and restart
                   the sequential downloader AT the seek target, so the
                   bytes right after the new position are fetched first
                   (greedy forward from the new point). Bytes the old
                   downloader wrote remain valid on disk — record them. */
                if (stream_downloader_watermark(g_downloader) > 0)
                    cache_seglist_add(&g_segs, seq_start,
                                      stream_downloader_watermark(g_downloader));
                stream_downloader_destroy(g_downloader);
                g_downloader = NULL;
                StreamDownloader *nd = stream_downloader_create_resume(
                    g_stream_url, g_part_path, target_byte);
                int restarted = 0;
                if (nd && stream_downloader_start(nd) == 0) {
                    g_downloader = nd;
                    restarted = 1;   /* downloader is live; reads wait for it */
                    /* Brief initial wait so the first read at the target
                       isn't a total stall. Deliberately short (not a multi-
                       second block): on a slow link it just times out and
                       playback blocks inside wait_more_data's own 500ms per-
                       read poll, keeping the playback thread responsive
                       during repeated seeks. */
                    int w = stream_downloader_wait_watermark(nd, 1, 500);
                    if (w != 1)
                        LOG_WARN("Seek resume data wait (500ms) not "
                                 "satisfied — playback will wait for the "
                                 "downloader to cover %d s", seek_sec);
                } else if (nd) {
                    stream_downloader_destroy(nd);
                }
                if (restarted) {
                    /* The downloader now covers the seek target — jump there
                       directly instead of clamping to the old continuous
                       prefix (which would snap back to the start). Use the
                       byte seek, NOT the timestamp seek: the latter
                       AVSEEK_FLAG_BACKWARD-backtracks to a frame boundary,
                       which can land in the structurally-empty region
                       between the old downloader's end and the new target —
                       a "dead zone" no downloader fills, stalling playback.
                       The byte seek lands exactly on the new downloader's
                       start, where data is (or is about to be) available. */
                    int ok = (ffstream_seek_bytes(ffstream, target_byte) == 0);
                    if (ok != 0)
                        /* demuxer without AVSEEK_FLAG_BYTE support — fall
                           back to the timestamp seek (rare; only used if
                           the byte seek rejects the offset) */
                        ok = (ffstream_seek(ffstream, seek_sec) == 0);
                    if (ok)
                        *current_frame = seek_sec * samplerate;
                    return;
                }
                /* restart failed — the watermark kept whatever it had */
                avail_bytes = growing_avail_bytes();
                avail = (int)(avail_bytes * 8 / br);
            }
        }
        if (avail >= 0 && seek_sec > avail)
            target = avail * samplerate;
        int ok = (ffstream_seek(ffstream, target / samplerate) == 0);
        if (ok)
            *current_frame = target;
        return;
    }
    if (total_frames <= 0) return;
    if (target >= total_frames) target = total_frames - 1;
    int ok = 1;
    if (ffstream)
        ok = (ffstream_seek(ffstream, target / samplerate) == 0);
    if (decoder)
        decoder_seek(decoder, target);
    if (ok)
        *current_frame = target;
}

static void* playback_thread(void *arg) {
    (void)arg;
    LOG_INFO("Playback thread started");

    /* Startup: reconcile the cache index against disk before any stream
       opens. No downloader exists yet, so leftover .part files are crash
       residue and entries pointing at missing/truncated files are dropped. */
    audio_cache_reconcile();

    Decoder      *decoder = NULL;
    AudioOutput  *audio   = NULL;
    PlayState     state   = PS_STOPPED;
    Config *cfg = config_global();
    int     samplerate  = cfg ? config_get_int(cfg, "audio.sample_rate", 44100) : 44100;
    int     channels    = cfg ? config_get_int(cfg, "audio.channels", 2) : 2;
    int     current_frame = 0;
    int     total_frames  = -1;

    int16_t *pcm_buf = (int16_t*)malloc(
        (size_t)FRAMES_PER_CHUNK * 2 * sizeof(int16_t));
    if (!pcm_buf) {
        LOG_ERROR("OOM in playback thread");
        return NULL;
    }

    /* ── Spectrum analysis buffer ──────────────────── */
    int16_t *spectrum_buf = (int16_t*)calloc(
        (size_t)SPECTRUM_FFT_SIZE, sizeof(int16_t));
    /* Overlapping window: FFT_SIZE=1024, HOP=512.
       Every 512 new samples we shift the buffer and run FFT.
       Gives 43 Hz/bin resolution at ~86 Hz update rate. */
    #define SPECTRUM_HOP (SPECTRUM_FFT_SIZE / 2)
    int spectrum_pending = 0;
    int16_t spectrum_tmp[SPECTRUM_HOP];
    if (!spectrum_buf) {
        LOG_ERROR("OOM for spectrum buffer");
        free(pcm_buf);
        return NULL;
    }
    float spectrum_bands[SPECTRUM_BANDS];
    FFStream *ffstream = NULL;

    while (g_running) {
        /* ── Wait for next command (blocking when not playing) ── */
        Command cmd;
        bool has_cmd;

        if (state == PS_PLAYING) {
            has_cmd = cmd_queue_try_pop(&g_cmd_queue, &cmd);
        } else {
            cmd = cmd_queue_pop(&g_cmd_queue);
            has_cmd = true;
        }

        if (has_cmd) {
            switch (cmd.type) {
            case CMD_QUIT:
                goto cleanup;
            case CMD_STOP:
                if (state == PS_STOPPED) continue; /* guard feedback loop */
                if (ffstream) {
                    cache_keep_partial(ffstream);
                    ffstream_close(ffstream);
                    ffstream = NULL;
                }
                if (decoder) { decoder_close(decoder); decoder = NULL; }
                audio_teardown(audio); audio = NULL;
                state = PS_STOPPED;
                current_frame = 0;
                /* Don't publish EV_PLAYBACK_STOP here — the app already set
                   StateStore when it sent the command. Publishing it now would
                   race with a subsequent EV_PLAYBACK_START (e.g. from Space
                   pressed right after Stop) and clobber the Playing state. */
                continue;

            case CMD_PLAY: {
                int rc = open_stream(cmd.path, &ffstream, &decoder, &audio,
                                     &samplerate, &channels, &total_frames);
                if (rc == 1) {
                    /* unplayable (no copyright / delisted): skip to the
                       next track instead of stalling */
                    event_bus_publish(EV_PLAYBACK_SKIP, NULL, 0);
                    continue;
                }
                if (rc != 0) {
                    event_bus_publish(EV_PLAYBACK_ERROR, NULL, 0);
                    continue;
                }
                current_frame = 0;
                state = PS_PLAYING;
                continue;
            }

            case CMD_RELOAD: {
                /* in-place quality switch: reopen the same netease track at
                   the now-resolved quality and resume from seek_frame sec,
                   keeping the playing/paused state. */
                bool was_paused = (state == PS_PAUSED);
                int rc = open_stream(cmd.path, &ffstream, &decoder, &audio,
                                     &samplerate, &channels, &total_frames);
                if (rc != 0) {
                    LOG_ERROR("Reload failed for %s", cmd.path);
                    event_bus_publish(EV_PLAYBACK_ERROR, NULL, 0);
                    continue;
                }
                if (cmd.seek_frame > 0)
                    do_seek(ffstream, decoder, samplerate, total_frames,
                            cmd.seek_frame, &current_frame);
                else
                    current_frame = 0;
                state = was_paused ? PS_PAUSED : PS_PLAYING;
                if (was_paused && audio) {
                    audio_output_flush(audio);
                    audio_output_pause(audio);
                }
                continue;
            }

            case CMD_PAUSE:
                if (state == PS_PLAYING) {
                    state = PS_PAUSED;
                    if (audio) {
                        audio_output_flush(audio);
                        audio_output_pause(audio);
                    }
                    event_bus_publish(EV_PLAYBACK_PAUSE, NULL, 0);
                }
                continue;
            case CMD_RESUME:
                if (state == PS_PAUSED) {
                    state = PS_PLAYING;
                    if (audio) audio_output_resume(audio);
                    event_bus_publish(EV_PLAYBACK_RESUME, NULL, 0);
                }
                continue;
            case CMD_SEEK:
                if (total_frames > 0)
                    do_seek(ffstream, decoder, samplerate, total_frames,
                            cmd.seek_frame, &current_frame);
                continue;
            default:
                break;
            }
        }

        /* ── Decode + handle commands inline ── */
        /* This loop runs while playing. Commands are peeked (non-blocking)
           and processed inline — the loop never exits just to "check for
           commands", avoiding audio starvation. */
        int64_t last_progress_ms = 0;

        while (state == PS_PLAYING && (ffstream || decoder) && audio && g_running) {
            /* Peek for commands (non-blocking) */
            Command icmd;
            if (cmd_queue_try_pop(&g_cmd_queue, &icmd)) {
                switch (icmd.type) {
                case CMD_PAUSE:
                    state = PS_PAUSED;
                    if (audio) {
                        audio_output_flush(audio);
                        audio_output_pause(audio);
                    }
                    event_bus_publish(EV_PLAYBACK_PAUSE, NULL, 0);
                    goto next_song;
                case CMD_RESUME:
                    /* should not happen while playing */
                    break;
                case CMD_STOP:
                    if (state == PS_STOPPED) goto next_song;
                    if (ffstream) {
                        cache_keep_partial(ffstream);
                        ffstream_close(ffstream);
                        ffstream = NULL;
                    }
                    if (decoder) { decoder_close(decoder); decoder = NULL; }
                    audio_teardown(audio); audio = NULL;
                    state = PS_STOPPED;
                    current_frame = 0;
                    /* Don't publish EV_PLAYBACK_STOP here — same race
                       condition as the outer loop handler. */
                    goto next_song;
                case CMD_PLAY:
                    /* switch to new track */
                    if (ffstream) {
                        cache_keep_partial(ffstream);
                        ffstream_close(ffstream);
                        ffstream = NULL;
                    }
                    if (decoder) { decoder_close(decoder); decoder = NULL; }
                    audio_teardown(audio); audio = NULL;
                    state = PS_STOPPED;
                    /* push a fresh CMD_PLAY for the outer loop */
                    cmd_queue_push(&g_cmd_queue, &icmd);
                    goto next_song;
                case CMD_RELOAD:
                    /* in-place quality switch while playing: reopen the
                       stream via the outer loop, preserving seek + state */
                    if (ffstream) {
                        cache_keep_partial(ffstream);
                        ffstream_close(ffstream);
                        ffstream = NULL;
                    }
                    if (decoder) { decoder_close(decoder); decoder = NULL; }
                    audio_teardown(audio); audio = NULL;
                    state = PS_STOPPED;
                    cmd_queue_push(&g_cmd_queue, &icmd);
                    goto next_song;
                case CMD_SEEK:
                    if (total_frames > 0) {
                        do_seek(ffstream, decoder, samplerate, total_frames,
                                icmd.seek_frame, &current_frame);
                        /* flush residual audio from output buffer */
                        if (audio) audio_output_flush(audio);
                        /* force progress update — now_ms went backward */
                        last_progress_ms = -PROGRESS_INTERVAL_MS;
                    }
                    break;
                case CMD_QUIT:
                    goto cleanup;
                default:
                    break;
                }
            }

            /* Decode next chunk */
            int frames = ffstream ? ffstream_decode(ffstream, pcm_buf, FRAMES_PER_CHUNK)
                                    : decoder_decode(decoder, pcm_buf, FRAMES_PER_CHUNK);
            if (frames <= 0) {
                /* end of stream — finalize the recording as a cache entry.
                   In growing-file mode the decode only reports EOF after
                   the downloader finished (see wait_more_data), so reaching
                   this branch with a failed downloader means a mid-stream
                   network error, not a clean finish: surface it as an
                   error event instead of silently "finishing" the track. */
                int playback_err = 0;
                if (ffstream && g_rec_active) {
                    if (g_downloader) {
                        /* growing-file mode: the downloader is the writer */
                        if (stream_downloader_done(g_downloader) &&
                            !stream_downloader_failed(g_downloader)) {
                            /* download finished cleanly → commit complete */
                            if (g_reuse_cache && g_part_path) {
                                /* the file is already the final cache entry —
                                   commit in place (no rename) */
                                audio_cache_commit(g_rec_song, g_part_path,
                                                   g_rec_level, 1, NULL);
                                free(g_part_path);
                                g_part_path = NULL;
                            } else {
                                char *final = audio_cache_final_path(
                                    g_rec_song, ffstream_media_ext(ffstream));
                                if (final) {
                                    if (g_part_path) {
                                        if (rename_utf8(g_part_path, final) == 0)
                                            audio_cache_commit(g_rec_song,
                                                               final,
                                                               g_rec_level,
                                                               1, NULL);
                                        else
                                            remove_utf8(g_part_path);
                                        free(g_part_path);
                                        g_part_path = NULL;
                                    }
                                    free(final);
                                }
                            }
                        } else {
                            /* download failed/interrupted → keep the
                               received bytes as a partial cache entry and
                               report the failure */
                            playback_err = stream_downloader_failed(g_downloader);
                            int we = stream_downloader_write_errno(g_downloader);
#ifdef EDQUOT
                            if (we == ENOSPC || we == EDQUOT)
#else
                            if (we == ENOSPC)
#endif
                                LOG_ERROR("Disk full while downloading %s "
                                          "(errno %d) — cache kept partial",
                                          g_rec_song, we);
                            else
                                LOG_WARN("Download failed for %s", g_rec_song);
                            cache_keep_partial(ffstream);
                        }
                        stream_downloader_destroy(g_downloader);
                        g_downloader = NULL;
                    } else {
                        /* legacy recorder mode (partial-continuation
                           backfill commits the whole track) */
                        char *final = audio_cache_final_path(
                            g_rec_song, ffstream_media_ext(ffstream));
                        if (final) {
                            if (ffstream_recorder_commit(ffstream, final) == 0)
                                audio_cache_commit(g_rec_song, final,
                                                   g_rec_level, 1, NULL);
                            free(final);
                        }
                    }
                    g_rec_active = 0;
                }
                state = PS_STOPPED;
                event_bus_publish(playback_err ? EV_PLAYBACK_ERROR
                                               : EV_PLAYBACK_FINISH, NULL, 0);
                /* release the stream resources now — holding them until the
                   next command keeps the file handle + decoder memory alive
                   while stopped */
                if (ffstream) { ffstream_close(ffstream); ffstream = NULL; }
                if (decoder)  { decoder_close(decoder);  decoder = NULL; }
                audio_teardown(audio); audio = NULL;
                break;
            }

            int written = audio_output_write(audio, pcm_buf, frames);
            if (written > 0)
                current_frame += written;

            /* ── Spectrum capture (overlapping window) ── */
            {
                int to_copy = (frames > written) ? written : frames;
                if (to_copy > 0) {
                    /* Mix stereo to mono, batch into temp buffer */
                    int need = SPECTRUM_HOP - spectrum_pending;
                    if (need > to_copy) need = to_copy;
                    int dst = spectrum_pending;
                    for (int i = 0; i < need; i++, dst++) {
                        int s = (int)pcm_buf[i * 2];
                        if (channels >= 2)
                            s = (s + (int)pcm_buf[i * 2 + 1]) / 2;
                        /* Average of two int16 samples stays within int16
                           range, so no clamping is needed here. */
                        spectrum_tmp[dst] = (int16_t)s;
                    }
                    spectrum_pending += need;
                }

                /* Every HOP new samples slide the window and run FFT */
                if (spectrum_pending >= SPECTRUM_HOP) {
                    spectrum_pending = 0;

                    /* Shift out oldest HOP samples */
                    memmove(spectrum_buf,
                            spectrum_buf + SPECTRUM_HOP,
                            (SPECTRUM_FFT_SIZE - SPECTRUM_HOP)
                            * sizeof(int16_t));
                    /* Copy HOP new samples to end */
                    memcpy(spectrum_buf + (SPECTRUM_FFT_SIZE - SPECTRUM_HOP),
                           spectrum_tmp,
                           SPECTRUM_HOP * sizeof(int16_t));

                    spectrum_process(spectrum_buf, spectrum_bands,
                                     SPECTRUM_BANDS);
                    event_bus_publish(EV_SPECTRUM_UPDATE,
                                      spectrum_bands,
                                      sizeof(spectrum_bands));
                }
            }

            /* Progress event — integer math avoids per-chunk float division. */
            int64_t now_ms = (int64_t)current_frame * 1000 / samplerate;
            if (now_ms - last_progress_ms >= PROGRESS_INTERVAL_MS) {
                last_progress_ms = now_ms;
                long br = effective_bitrate(ffstream, current_frame,
                                            samplerate);
                /* Growing-file mode: the probe-time duration was estimated
                   from the partially downloaded file and is shorter than the
                   real track. Track the true length from (on-disk size /
                   the effective bitrate — declared, or measured for streams
                   without one) so the progress bar and seek bounds grow with
                   the download instead of overflowing at a wrong end. */
                if (ffstream && g_downloader &&
                    ffstream_growing(ffstream) && g_part_path && br > 0) {
                    /* Prefer the declared total (Content-Length) so the
                       progress bar shows the true track length from the
                       start; fall back to the continuous playable bytes
                       while growing. The raw on-disk size is deliberately
                       NOT used here: a restarted downloader fseeks past
                       holes, inflating the file size, and it never shrinks,
                       so a progress-bar total derived from it would overshoot
                       and stay wrong. growing_avail_bytes() is the true
                       contiguous download and converges as the file fills. */
                    int64_t total_bytes = stream_downloader_total_size(g_downloader);
                    if (total_bytes <= 0)
                        total_bytes = growing_avail_bytes();
                    if (total_bytes > 0) {
                        int64_t frames = total_bytes * 8 * samplerate / br;
                        if (frames > total_frames)
                            total_frames = (int)frames;
                    }
                }
                /* Send exact frames for smooth progress bar. The 4th slot
                   carries the stream's average bitrate (bits/sec; 0 when
                   unknown, e.g. local files decoded without ffstream) so the
                   UI can show it on the status bar — the same effective
                   bitrate shared with the growing correction above. */
                int progress_data[4] = {
                    current_frame,          /* exact frame position */
                    total_frames,           /* total frames          */
                    samplerate,             /* for time calculation  */
                    (int)(br > 0 ? br : 0)  /* bitrate bits/sec       */
                };
                event_bus_publish(EV_PROGRESS_UPDATE,
                                  progress_data, sizeof(progress_data));
            }
        }

next_song:
        ; /* fall through to outer loop — wait for next command */
    }

cleanup:
    if (ffstream) {
        cache_keep_partial(ffstream);
        ffstream_close(ffstream);
        ffstream = NULL;
    }
    if (decoder) decoder_close(decoder);
    audio_teardown(audio);
    free(spectrum_buf);
    free(pcm_buf);
    LOG_INFO("Playback thread ended");
    return NULL;
}

/* ── Init / Shutdown ────────────────────────────────── */
int playback_coordinator_init(void) {
    g_running = true;
    cmd_queue_init(&g_cmd_queue);

    /* subscribe to events */
    event_bus_subscribe(EV_APP_SHUTDOWN, on_app_shutdown, NULL);
    event_bus_subscribe(EV_PLAYBACK_START, on_play_start, NULL);
    event_bus_subscribe(EV_PLAYBACK_PAUSE, on_play_pause, NULL);
    event_bus_subscribe(EV_PLAYBACK_RESUME, on_play_resume, NULL);
    event_bus_subscribe(EV_PLAYBACK_STOP, on_play_stop, NULL);
    event_bus_subscribe(EV_BUFFERING_UPDATE, on_seek, NULL); /* reuse for seek */
    event_bus_subscribe(EV_PLAYBACK_RELOAD, on_play_reload, NULL);

    if (pthread_create(&g_thread, NULL, playback_thread, NULL) != 0) {
        LOG_ERROR("Failed to create playback thread");
        g_running = false;
        return -1;
    }
    LOG_INFO("Playback coordinator initialized");
    return 0;
}

void playback_coordinator_shutdown(void) {
    Command cmd = {.type = CMD_QUIT};
    cmd_queue_push(&g_cmd_queue, &cmd);
    pthread_join(g_thread, NULL);
    LOG_INFO("Playback coordinator shutdown");
}
