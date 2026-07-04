#include "playback_coordinator.h"
#include "core/decoder_manager.h"
#include "core/audio_output_mgr.h"
#include "core/music_source.h"
#include "core/music_source_manager.h"
#include "infra/config.h"
#include "core/event_bus.h"
#include "infra/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>

#define DIAG(msg) write(STDERR_FILENO, msg "\n", sizeof(msg))

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

/* ── Netease streaming temp file cleanup ────────────── */
static pid_t g_dl_child = 0;
static char  g_dl_path[2048] = "";

static void cleanup_dl(void) {
    if (g_dl_child > 0) {
        waitpid(g_dl_child, NULL, WNOHANG);
        g_dl_child = 0;
    }
    if (g_dl_path[0]) {
        unlink(g_dl_path);
        g_dl_path[0] = '\0';
    }
}

/* ── Event bus handlers ────────────────────────────── */
static void on_app_startup(const BusEvent *ev, void *ud) { (void)ev; (void)ud; }
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
    cmd_queue_push(&g_cmd_queue, &cmd);
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
                cleanup_dl();
                if (state == PS_STOPPED) continue; /* guard feedback loop */
                if (decoder) { decoder_close(decoder); decoder = NULL; }
                if (audio)   { audio_output_destroy(audio); audio = NULL; }
                state = PS_STOPPED;
                current_frame = 0;
                /* Don't publish EV_PLAYBACK_STOP here — the app already set
                   StateStore when it sent the command. Publishing it now would
                   race with a subsequent EV_PLAYBACK_START (e.g. from Space
                   pressed right after Stop) and clobber the Playing state. */
                continue;

            case CMD_PLAY: {
                cleanup_dl();
                if (decoder) { decoder_close(decoder); decoder = NULL; }
                if (audio)   { audio_output_destroy(audio); audio = NULL; }

                /* Resolve netease streaming URL via fifo for non-local paths */
                char resolved_path[2048];
                const char *play_path = cmd.path;
                int is_local = (cmd.path[0] == '/' || cmd.path[0] == '~' || strchr(cmd.path, '.'));
                if (!is_local) {
                    fprintf(stderr, "DIAG_PB: non-local path='%s', resolving...\n", cmd.path);
                    char url_buf[2048] = {0};
                    /* Fetch song URL via popen — bypasses music_source_get */
                    char cli_cmd[2048];
                    snprintf(cli_cmd, sizeof(cli_cmd),
                             "netease-cli song-url \"%s\" standard 2>/dev/null", cmd.path);
                    FILE *fp = popen(cli_cmd, "r");
                    if (fp) {
                        char jbuf[4096] = {0};
                        size_t nr = fread(jbuf, 1, sizeof(jbuf)-1, fp);
                        pclose(fp);
                        fprintf(stderr, "DIAG_PB: song-url returned %zu bytes\n", nr);
                        /* Extract URL from {data:[{url:"..."}]} */
                        const char *k = nr > 0 ? strstr(jbuf, "\"url\"") : NULL;
                        if (k) {
                            k += 5;
                            while (*k == ':' || *k == ' ' || *k == '\t') k++;
                            if (*k == '"') {
                                k++;
                                const char *end = k;
                                while (*end && *end != '"') end++;
                                size_t ulen = (size_t)(end - k);
                                if (ulen > 0 && ulen < sizeof(url_buf)-1) {
                                    memcpy(url_buf, k, ulen);
                                    url_buf[ulen] = '\0';
                                }
                            }
                        }
                    }
                    if (url_buf[0]) {
                        fprintf(stderr, "DIAG_PB: got url='%.60s'\n", url_buf);
                        snprintf(resolved_path, sizeof(resolved_path),
                                 "/tmp/netune_%s.mp3", cmd.path);
                        unlink(resolved_path);
                        mkfifo(resolved_path, 0600);
                        pid_t child = fork();
                        if (child == 0) {
                            execl("/usr/bin/curl", "curl", "-sL", url_buf,
                                  "--max-time", "30", "-o", resolved_path, NULL);
                            _exit(1);
                        }
                        if (child > 0) {
                            g_dl_child = child;
                            snprintf(g_dl_path, sizeof(g_dl_path), "%s", resolved_path);
                            play_path = resolved_path;
                            LOG_INFO("Streaming netease: %s", cmd.path);
                        } else {
                            unlink(resolved_path);
                            LOG_ERROR("fork failed for netease stream %s", cmd.path);
                        }
                    } else {
                        fprintf(stderr, "DIAG_PB: get_play_url failed or empty url\n");
                        LOG_WARN("No play URL for %s", cmd.path);
                    }
                }

                decoder = decoder_open(play_path);
                if (!decoder) {
                    cleanup_dl();
                    LOG_ERROR("Cannot open: %s", cmd.path);
                    event_bus_publish(EV_PLAYBACK_ERROR, NULL, 0);
                    continue;
                }
                DecoderInfo info;
                decoder_get_info(decoder, &info);
                samplerate   = info.sample_rate;
                channels     = info.channels;
                total_frames = info.total_frames;
                current_frame = 0;

                audio = audio_output_create(samplerate, channels);
                if (!audio) {
                    decoder_close(decoder);
                    decoder = NULL;
                    event_bus_publish(EV_PLAYBACK_ERROR, NULL, 0);
                    continue;
                }

                state = PS_PLAYING;
                /* Don't publish EV_PLAYBACK_START here — that would trigger
                   on_play_start() again, causing a CMD_PLAY feedback loop.
                   The UI already set StateStore to Playing when it sent the
                   command; this thread just starts actual decoding. */
                continue;
            }

            case CMD_PAUSE:
                if (state == PS_PLAYING) {
                    state = PS_PAUSED;
                    event_bus_publish(EV_PLAYBACK_PAUSE, NULL, 0);
                }
                continue;
            case CMD_RESUME:
                if (state == PS_PAUSED) {
                    state = PS_PLAYING;
                    event_bus_publish(EV_PLAYBACK_RESUME, NULL, 0);
                }
                continue;
            case CMD_SEEK:
                /* seek while paused — update decoder position */
                if (decoder && total_frames > 0) {
                    int target = cmd.seek_frame * samplerate;
                    if (target < 0) target = 0;
                    if (target >= total_frames)
                        target = total_frames - 1;
                    decoder_seek(decoder, target);
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

        while (state == PS_PLAYING && decoder && audio && g_running) {
            /* Peek for commands (non-blocking) */
            Command icmd;
            if (cmd_queue_try_pop(&g_cmd_queue, &icmd)) {
                switch (icmd.type) {
                case CMD_PAUSE:
                    state = PS_PAUSED;
                    event_bus_publish(EV_PLAYBACK_PAUSE, NULL, 0);
                    goto next_song;
                case CMD_RESUME:
                    /* should not happen while playing */
                    break;
                case CMD_STOP:
                    cleanup_dl();
                    if (state == PS_STOPPED) goto next_song;
                    if (decoder) { decoder_close(decoder); decoder = NULL; }
                    if (audio)   { audio_output_destroy(audio); audio = NULL; }
                    state = PS_STOPPED;
                    current_frame = 0;
                    /* Don't publish EV_PLAYBACK_STOP here — same race
                       condition as the outer loop handler. */
                    goto next_song;
                case CMD_PLAY:
                    /* switch to new track */
                    if (decoder) { decoder_close(decoder); decoder = NULL; }
                    if (audio)   { audio_output_destroy(audio); audio = NULL; }
                    state = PS_STOPPED;
                    /* push a fresh CMD_PLAY for the outer loop */
                    cmd_queue_push(&g_cmd_queue, &icmd);
                    goto next_song;
                case CMD_SEEK:
                    if (decoder && total_frames > 0) {
                        int target = icmd.seek_frame * samplerate;
                        if (target < 0) target = 0;
                        if (target >= total_frames)
                            target = total_frames - 1;
                        decoder_seek(decoder, target);
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
            int frames = decoder_decode(decoder, pcm_buf, FRAMES_PER_CHUNK);
            if (frames <= 0) {
                state = PS_STOPPED;
                event_bus_publish(EV_PLAYBACK_FINISH, NULL, 0);
                break;
            }

            int written = audio_output_write(audio, pcm_buf, frames);
            if (written > 0)
                current_frame += written;

            /* Progress event */
            int64_t now_ms = (int64_t)((double)current_frame / samplerate * 1000);
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
    cleanup_dl();
    if (decoder) decoder_close(decoder);
    if (audio)   audio_output_destroy(audio);
    free(pcm_buf);
    LOG_INFO("Playback thread ended");
    return NULL;
}

/* ── Init / Shutdown ────────────────────────────────── */
int playback_coordinator_init(void) {
    g_running = true;
    cmd_queue_init(&g_cmd_queue);

    /* subscribe to events */
    event_bus_subscribe(EV_APP_STARTUP, on_app_startup, NULL);
    event_bus_subscribe(EV_APP_SHUTDOWN, on_app_shutdown, NULL);
    event_bus_subscribe(EV_PLAYBACK_START, on_play_start, NULL);
    event_bus_subscribe(EV_PLAYBACK_PAUSE, on_play_pause, NULL);
    event_bus_subscribe(EV_PLAYBACK_RESUME, on_play_resume, NULL);
    event_bus_subscribe(EV_PLAYBACK_STOP, on_play_stop, NULL);
    event_bus_subscribe(EV_BUFFERING_UPDATE, on_seek, NULL); /* reuse for seek */

    pthread_create(&g_thread, NULL, playback_thread, NULL);
    LOG_INFO("Playback coordinator initialized");
    return 0;
}

void playback_coordinator_shutdown(void) {
    Command cmd = {.type = CMD_QUIT};
    cmd_queue_push(&g_cmd_queue, &cmd);
    pthread_join(g_thread, NULL);
    LOG_INFO("Playback coordinator shutdown");
}
