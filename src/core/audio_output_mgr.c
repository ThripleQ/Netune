#include "audio_output_mgr.h"
#include "infra/log.h"
#include "infra/config.h"
#include <stdlib.h>

#define MAX_BACKENDS 4

static AudioOutputBackend *g_backends[MAX_BACKENDS];
static int                 g_count = 0;

static AudioOutputBackend *g_active = NULL;

int audio_output_register_backend(AudioOutputBackend *backend) {
    if (!backend || !backend->name) return -1;
    if (g_count >= MAX_BACKENDS) return -1;
    g_backends[g_count++] = backend;
    LOG_INFO("Audio backend registered: %s", backend->name);
    return 0;
}

/* built-in backends */
extern AudioOutputBackend g_alsa_backend;

static void register_builtins(void) {
    static int done = 0;
    if (done) return;
    audio_output_register_backend(&g_alsa_backend);
    done = 1;
}

struct AudioOutput {
    int sample_rate;
    int channels;
};

AudioOutput* audio_output_create(int sample_rate, int channels) {
    register_builtins();

    if (g_active) {
        LOG_WARN("Audio output already active");
        return NULL;
    }

    /* probe each backend, use first working */
    AudioOutputBackend *chosen = NULL;
    for (int i = 0; i < g_count; i++) {
        if (g_backends[i]->probe && g_backends[i]->probe()) {
            chosen = g_backends[i];
            break;
        }
    }

    if (!chosen) {
        LOG_ERROR("No audio backend available");
        return NULL;
    }

    Config *gcfg = config_global();
    AudioConfig cfg;
    cfg.sample_rate = sample_rate;
    cfg.channels = channels;
    cfg.bits_per_sample = 16;
    cfg.buffer_frames = gcfg ? config_get_int(gcfg, "audio.buffer_frames", 4096) : 4096;

    if (chosen->init && chosen->init(&cfg) != 0) {
        LOG_ERROR("Backend %s init failed", chosen->name);
        return NULL;
    }

    g_active = chosen;
    LOG_INFO("Audio output active: %s (%dHz %dch)", chosen->name,
             sample_rate, channels);

    AudioOutput *ao = (AudioOutput*)malloc(sizeof(AudioOutput));
    ao->sample_rate = sample_rate;
    ao->channels = channels;
    return ao;
}

void audio_output_destroy(AudioOutput *ao) {
    if (!ao) return;
    if (g_active && g_active->shutdown)
        g_active->shutdown();
    g_active = NULL;
    free(ao);
    LOG_DEBUG("Audio output destroyed");
}

int audio_output_write(AudioOutput *ao, const int16_t *pcm, int frames) {
    if (!ao || !g_active || !g_active->write) return -1;
    return g_active->write(
        (const uint8_t*)pcm,
        (size_t)frames * ao->channels * sizeof(int16_t));
}

int audio_output_delay_us(AudioOutput *ao, uint64_t *delay_us) {
    if (!ao || !g_active || !g_active->get_delay_us) return -1;
    return g_active->get_delay_us(delay_us);
}

int audio_output_flush(AudioOutput *ao) {
    (void)ao;
    if (!g_active) return -1;
    if (g_active->flush) return g_active->flush();
    return -1;
}
