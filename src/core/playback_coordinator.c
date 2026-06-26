#include "playback_coordinator.h"
#include "core/decoder_manager.h"
#include "core/audio_output_mgr.h"
#include "core/event_bus.h"
#include "infra/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

/* ── Constants ──────────────────────────────────────── */
#define FRAMES_PER_CHUNK 4096
#define PROGRESS_INTERVAL_MS 250

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

static void* playback_thread(void *arg) {
    (void)arg;
    LOG_INFO("Playback thread started");

    Decoder      *decoder = NULL;
    AudioOutput  *audio   = NULL;
    PlayState     state   = PS_STOPPED;
    int           samplerate  = 44100;
    int           channels    = 2;
    int           current_frame = 0;
    int           total_frames  = -1;

    int16_t      *pcm_buf = (int16_t*)malloc(
                               (size_t)FRAMES_PER_CHUNK * 2 * sizeof(int16_t));
    if (!pcm_buf) {
        LOG_ERROR("OOM in playback thread");
        return NULL;
    }

    while (g_running) {
        Command cmd = cmd_queue_pop(&g_cmd_queue);

        switch (cmd.type) {
        case CMD_QUIT:
            goto cleanup;
        case CMD_STOP:
            if (decoder) { decoder_close(decoder); decoder = NULL; }
            if (audio)   { audio_output_destroy(audio); audio = NULL; }
            state = PS_STOPPED;
            current_frame = 0;
            event_bus_publish(EV_PLAYBACK_STOP, NULL, 0);
            break;

        case CMD_PLAY: {
            /* close previous */
            if (decoder) { decoder_close(decoder); decoder = NULL; }
            if (audio)   { audio_output_destroy(audio); audio = NULL; }

            decoder = decoder_open(cmd.path);
            if (!decoder) {
                LOG_ERROR("Cannot open: %s", cmd.path);
                event_bus_publish(EV_PLAYBACK_ERROR, NULL, 0);
                break;
            }
            DecoderInfo info;
            decoder_get_info(decoder, &info);
            samplerate  = info.sample_rate;
            channels    = info.channels;
            total_frames = info.total_frames;
            current_frame = 0;

            audio = audio_output_create(samplerate, channels);
            if (!audio) {
                decoder_close(decoder);
                decoder = NULL;
                event_bus_publish(EV_PLAYBACK_ERROR, NULL, 0);
                break;
            }

            state = PS_PLAYING;
            event_bus_publish(EV_PLAYBACK_START, NULL, 0);
            break;
        }

        case CMD_PAUSE:
            if (state == PS_PLAYING) {
                state = PS_PAUSED;
                event_bus_publish(EV_PLAYBACK_PAUSE, NULL, 0);
            }
            break;

        case CMD_RESUME:
            if (state == PS_PAUSED) {
                state = PS_PLAYING;
                event_bus_publish(EV_PLAYBACK_RESUME, NULL, 0);
            }
            break;

        case CMD_SEEK:
            if (decoder && total_frames > 0) {
                int target = cmd.seek_frame * samplerate;
                if (target < 0) target = 0;
                if (target >= total_frames) target = total_frames - 1;
                decoder_seek(decoder, target);
                current_frame = target;
            }
            break;

        case CMD_NONE:
            break;
        }

        /* ── Decode loop (while playing, yield to commands periodically) ── */
        int64_t last_progress_ms = 0;

        while (state == PS_PLAYING && decoder && audio && g_running) {
            int frames = decoder_decode(decoder, pcm_buf, FRAMES_PER_CHUNK);
            if (frames <= 0) {
                /* EOF */
                state = PS_STOPPED;
                event_bus_publish(EV_PLAYBACK_FINISH, NULL, 0);
                break;
            }

            int written = audio_output_write(audio, pcm_buf, frames);
            if (written > 0)
                current_frame += written;

            /* check for new commands (non-blocking) */
            pthread_mutex_lock(&g_cmd_queue.mutex);
            if (g_cmd_queue.count > 0) {
                pthread_mutex_unlock(&g_cmd_queue.mutex);
                break; /* exit decode loop to process command */
            }
            pthread_mutex_unlock(&g_cmd_queue.mutex);

            /* progress event */
            int64_t now_ms = (int64_t)((double)current_frame / samplerate * 1000);
            if (now_ms - last_progress_ms >= PROGRESS_INTERVAL_MS) {
                last_progress_ms = now_ms;
                /* pack progress as two ints: current_sec, total_sec */
                int progress_data[2] = {
                    current_frame / samplerate,
                    total_frames > 0 ? total_frames / samplerate : 0
                };
                event_bus_publish(EV_PROGRESS_UPDATE, progress_data, sizeof(progress_data));
            }
        }
    }

cleanup:
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
