#include "audio_output_mgr.h"
#include "infra/log.h"
#include "infra/config.h"
#include <stdlib.h>

#define MAX_BACKENDS 4

static AudioOutputBackend *g_backends[MAX_BACKENDS];
static int                 g_count = 0;

static AudioOutputBackend *g_active = NULL;
static AudioOutput       *g_active_ao = NULL;

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
    int volume;  /* 0-100, applied as software gain */
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
    ao->volume = 80;
    Config *gcfg2 = config_global();
    if (gcfg2) ao->volume = config_get_int(gcfg2, "audio.volume", 80);
    g_active_ao = ao;
    return ao;
}

void audio_output_destroy(AudioOutput *ao) {
    if (!ao) return;
    if (g_active && g_active->shutdown)
        g_active->shutdown();
    g_active = NULL;
    g_active_ao = NULL;
    free(ao);
    LOG_DEBUG("Audio output destroyed");
}

int audio_output_write(AudioOutput *ao, const int16_t *pcm, int frames) {
    if (!ao || !g_active || !g_active->write) return -1;

    size_t bytes = (size_t)frames * ao->channels * sizeof(int16_t);

    if (ao->volume < 100) {
        /* software volume: apply gain to PCM samples */
        size_t samples = (size_t)frames * ao->channels;
        int16_t *scaled = (int16_t*)malloc(bytes);
        if (!scaled) return -1;
        double gain = (double)ao->volume / 100.0;
        for (size_t i = 0; i < samples; i++) {
            double v = (double)pcm[i] * gain;
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            scaled[i] = (int16_t)v;
        }
        int rc = g_active->write((const uint8_t*)scaled, bytes);
        free(scaled);
        return rc;
    }

    return g_active->write((const uint8_t*)pcm, bytes);
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

/* Volume is stored in the AudioOutput struct and applied as
   software gain in audio_output_write(). Does NOT touch system mixer. */
int audio_output_set_volume(int vol) {
    if (!g_active_ao) return -1;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    g_active_ao->volume = vol;
    return 0;
}

int audio_output_get_volume(void) {
    if (!g_active_ao) return -1;
    return g_active_ao->volume;
}
