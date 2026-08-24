#include "playback_coordinator.h"
#include "core/decoder_manager.h"
#include "core/audio_output_mgr.h"
#include "core/music_source.h"
#include "core/music_source_manager.h"
#include "core/audio_cache.h"
#include "plugins/decoders/ffmpeg/ffmpeg_stream.h"
#include "plugins/music_sources/netease/netease_quality.h"
#include "infra/config.h"
#include "core/event_bus.h"
#include "core/spectrum.h"
#include "infra/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>


/* ── Constants ──────────────────────────────────────── */
#define FRAMES_PER_CHUNK 4096
#define PROGRESS_INTERVAL_MS 16

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
static int open_stream(const char *path,
                       FFStream **ffstream, Decoder **decoder,
                       AudioOutput **audio,
                       int *samplerate, int *channels, int *total_frames) {
    if (*ffstream) { ffstream_close(*ffstream); *ffstream = NULL; }
    if (*decoder)  { decoder_close(*decoder); *decoder = NULL; }
    audio_teardown(*audio); *audio = NULL;

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
        snprintf(g_rec_song, sizeof(g_rec_song), "%s", path);
        g_rec_level[0] = '\0';

        char *level = nq_resolve_level(path);
        if (!level) {
            LOG_WARN("No usable quality for %s", path);
            return 1;  /* unplayable — caller decides skip/error */
        }
        snprintf(g_rec_level, sizeof(g_rec_level), "%s", level);

        /* 1. cache hit → play the cached file directly (offline, the stream
              never touches the network) */
        char cache_path[1100];
        if (audio_cache_find(path, level, cache_path, sizeof(cache_path)) == 0) {
            *decoder = decoder_open(cache_path);
            if (!*decoder) {
                LOG_ERROR("Cannot open cached: %s", cache_path);
                free(level);
                return -1;
            }
            DecoderInfo info;
            decoder_get_info(*decoder, &info);
            *samplerate   = info.sample_rate;
            *channels     = info.channels;
            *total_frames = info.total_frames;
            audio_cache_touch(path);
        } else {
            /* 2. cache miss → stream from netease, recording raw bytes to a
                  .part file that is committed on natural end-of-stream */
            char url[2048] = {0};
            MusicSource *src = music_source_get("netease");
            if (!src || !src->get_play_url ||
                src->get_play_url(path, 0, url, sizeof(url)) != 0 || !url[0]) {
                LOG_WARN("No play URL for %s", path);
                free(level);
                return 1;  /* unplayable — caller decides skip/error */
            }
            LOG_INFO("Streaming netease: %s", path);
            int dur_sec = 0;
            char *part = audio_cache_part_path(path);
            *ffstream = ffstream_open_rec(url, part, samplerate, channels,
                                          &dur_sec);
            free(part);
            if (!*ffstream) {
                LOG_WARN("FFmpeg stream open failed %s", path);
                free(level);
                return -1;
            }
            *total_frames = (int64_t)dur_sec * (*samplerate);
            g_rec_active = ffstream_recording(*ffstream);
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

/* Apply a seek (seconds) to the current stream, updating current_frame. */
static void do_seek(FFStream *ffstream, Decoder *decoder, int samplerate,
                    int total_frames, int seek_sec, int *current_frame) {
    if (total_frames <= 0) return;
    int target = seek_sec * samplerate;
    if (target < 0) target = 0;
    if (target >= total_frames) target = total_frames - 1;
    int ok = 1;
    if (ffstream)
        ok = (ffstream_seek(ffstream, seek_sec) == 0);
    if (decoder)
        decoder_seek(decoder, target);
    if (ok)
        *current_frame = target;
}

static void* playback_thread(void *arg) {
    (void)arg;
    LOG_INFO("Playback thread started");

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
                if (total_frames > 0) {
                    int target = cmd.seek_frame * samplerate;
                    if (target < 0) target = 0;
                    if (target >= total_frames)
                        target = total_frames - 1;
                    int ok = 1;
                    if (ffstream)
                        ok = (ffstream_seek(ffstream, cmd.seek_frame) == 0);
                    if (decoder)
                        decoder_seek(decoder, target);
                    if (ok)
                        current_frame = target;
                }
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
                    if (ffstream) { ffstream_close(ffstream); ffstream = NULL; }
                    if (decoder) { decoder_close(decoder); decoder = NULL; }
                    audio_teardown(audio); audio = NULL;
                    state = PS_STOPPED;
                    current_frame = 0;
                    /* Don't publish EV_PLAYBACK_STOP here — same race
                       condition as the outer loop handler. */
                    goto next_song;
                case CMD_PLAY:
                    /* switch to new track */
                    if (ffstream) { ffstream_close(ffstream); ffstream = NULL; }
                    if (decoder) { decoder_close(decoder); decoder = NULL; }
                    audio_teardown(audio); audio = NULL;
                    state = PS_STOPPED;
                    /* push a fresh CMD_PLAY for the outer loop */
                    cmd_queue_push(&g_cmd_queue, &icmd);
                    goto next_song;
                case CMD_RELOAD:
                    /* in-place quality switch while playing: reopen the
                       stream via the outer loop, preserving seek + state */
                    if (ffstream) { ffstream_close(ffstream); ffstream = NULL; }
                    if (decoder) { decoder_close(decoder); decoder = NULL; }
                    audio_teardown(audio); audio = NULL;
                    state = PS_STOPPED;
                    cmd_queue_push(&g_cmd_queue, &icmd);
                    goto next_song;
                case CMD_SEEK:
                    if (total_frames > 0) {
                        int target = icmd.seek_frame * samplerate;
                        if (target < 0) target = 0;
                        if (target >= total_frames)
                            target = total_frames - 1;
                        int ok = 1;
                        if (ffstream)
                            ok = (ffstream_seek(ffstream, icmd.seek_frame) == 0);
                        if (decoder)
                            decoder_seek(decoder, target);
                        if (ok)
                            current_frame = target;
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
                /* natural end-of-stream — finalize the recording as a
                   cache entry (the whole stream has been received) */
                if (ffstream && g_rec_active) {
                    char *final = audio_cache_final_path(
                        g_rec_song, ffstream_media_ext(ffstream));
                    if (final) {
                        if (ffstream_recorder_commit(ffstream, final) == 0)
                            audio_cache_commit(g_rec_song, final, g_rec_level);
                        free(final);
                    }
                    g_rec_active = 0;
                }
                state = PS_STOPPED;
                event_bus_publish(EV_PLAYBACK_FINISH, NULL, 0);
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
                /* Send exact frames for smooth progress bar */
                int progress_data[3] = {
                    current_frame,          /* exact frame position */
                    total_frames,           /* total frames          */
                    samplerate              /* for time calculation  */
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
