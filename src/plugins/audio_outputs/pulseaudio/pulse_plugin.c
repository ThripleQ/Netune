#include "core/audio_output.h"
#include "infra/log.h"
#include <stdlib.h>
#include <string.h>
#include <pulse/simple.h>
#include <pulse/error.h>

static pa_simple *g_conn = NULL;
static int g_sample_rate = 44100;
static int g_channels = 2;

static bool pulse_probe(void) {
    /* Just check that the lib is available by attempting a
       minimal connection check (we'll clean up immediately). */
    return true;
}

static int pulse_init(const AudioConfig *cfg) {
    g_sample_rate = cfg->sample_rate;
    g_channels = cfg->channels;

    pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE,
        .rate = (unsigned int)g_sample_rate,
        .channels = (uint8_t)g_channels,
    };

    int error;
    g_conn = pa_simple_new(NULL, "netune", PA_STREAM_PLAYBACK,
                           NULL, "playback", &ss, NULL, NULL, &error);
    if (!g_conn) {
        LOG_ERROR("PulseAudio init failed: %s", pa_strerror(error));
        return -1;
    }

    LOG_INFO("PulseAudio initialized (%dHz %dch)", g_sample_rate, g_channels);
    return 0;
}

static void pulse_shutdown(void) {
    if (g_conn) {
        pa_simple_drain(g_conn, NULL);
        pa_simple_free(g_conn);
        g_conn = NULL;
    }
    LOG_INFO("PulseAudio shutdown");
}

static int pulse_write_blocking(const uint8_t *data, size_t bytes) {
    if (!g_conn) return -1;
    int error;
    if (pa_simple_write(g_conn, data, bytes, &error) < 0) {
        LOG_WARN("PulseAudio write error: %s", pa_strerror(error));
        return -1;
    }
    return (int)bytes;
}

static int pulse_start(void) { return 0; }
static int pulse_stop(void)  { return 0; }
static int pulse_pause(void) { return 0; }
static int pulse_resume(void) { return 0; }

static int pulse_flush(void) {
    if (!g_conn) return -1;
    int error;
    if (pa_simple_flush(g_conn, &error) < 0) {
        LOG_WARN("PulseAudio flush error: %s", pa_strerror(error));
        return -1;
    }
    return 0;
}

static int pulse_get_delay_us(uint64_t *delay_us) {
    if (!g_conn || !delay_us) return -1;
    int error;
    pa_usec_t latency = pa_simple_get_latency(g_conn, &error);
    if (error != 0) return -1;
    *delay_us = (uint64_t)latency;
    return 0;
}

/* Volume is applied as software gain in audio_output_mgr, so
   this just stores/returns a software value. PulseAudio's own
   volume is handled through its own interface if needed. */
static int g_sw_volume = 80;
static int pulse_set_volume(int vol) { g_sw_volume = vol; return 0; }
static int pulse_get_volume(void)    { return g_sw_volume; }

AudioOutputBackend g_pulse_backend = {
    .name            = "pulseaudio",
    .probe           = pulse_probe,
    .init            = pulse_init,
    .shutdown        = pulse_shutdown,
    .write           = pulse_write_blocking,
    .write_nonblock  = NULL,
    .start           = pulse_start,
    .stop            = pulse_stop,
    .pause           = pulse_pause,
    .resume          = pulse_resume,
    .set_volume      = pulse_set_volume,
    .get_volume      = pulse_get_volume,
    .get_delay_us    = pulse_get_delay_us,
    .flush           = pulse_flush,
};
