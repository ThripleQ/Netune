#include "core/audio_output.h"
#include "infra/log.h"
#include <stdlib.h>
#include <SDL2/SDL.h>

static SDL_AudioDeviceID g_dev = 0;
static int g_sample_rate = 44100;
static int g_channels = 2;

static bool sdl_probe(void) {
    /* SDL_Init could be called multiple times, it's safe */
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        LOG_WARN("SDL audio init failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

static void sdl_audio_cb(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    /* We use SDL_QueueAudio for push model, so this callback
       is for the pull model (not used). Keep it silent. */
    SDL_memset(stream, 0, (size_t)len);
}

static int sdl_init(const AudioConfig *cfg) {
    g_sample_rate = cfg->sample_rate;
    g_channels = cfg->channels;

    SDL_AudioSpec want, have;
    SDL_memset(&want, 0, sizeof(want));
    want.freq = g_sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)g_channels;
    want.samples = (Uint16)(cfg->buffer_frames > 0 ? cfg->buffer_frames : 4096);
    want.callback = sdl_audio_cb;

    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) {
        LOG_ERROR("SDL open audio device failed: %s", SDL_GetError());
        return -1;
    }

    /* If format changed, warn but continue */
    if (have.format != AUDIO_S16SYS || have.channels != (Uint8)g_channels) {
        LOG_WARN("SDL audio format mismatch (requested %dch S16, got %dch 0x%x)",
                 g_channels, have.channels, have.format);
    }

    LOG_INFO("SDL audio initialized (%dHz %dch, device=%u)", g_sample_rate, g_channels, g_dev);
    return 0;
}

static void sdl_shutdown(void) {
    if (g_dev) {
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    LOG_INFO("SDL audio shutdown");
}

static int sdl_write_blocking(const uint8_t *data, size_t bytes) {
    if (!g_dev) return -1;
    if (SDL_QueueAudio(g_dev, data, (Uint32)bytes) < 0) {
        LOG_WARN("SDL queue audio failed: %s", SDL_GetError());
        return -1;
    }
    /* Keep the buffer small-ish by waiting if too much queued */
    while (SDL_GetQueuedAudioSize(g_dev) > 65536) {
        SDL_Delay(8);
    }
    return (int)bytes;
}

static int sdl_start(void) {
    if (g_dev) SDL_PauseAudioDevice(g_dev, 0);
    return 0;
}

static int sdl_stop(void) {
    if (g_dev) SDL_PauseAudioDevice(g_dev, 1);
    return 0;
}

static int sdl_pause(void) { return sdl_stop(); }
static int sdl_resume(void) { return sdl_start(); }

static int sdl_flush(void) {
    if (g_dev) SDL_ClearQueuedAudio(g_dev);
    return 0;
}

static int sdl_get_delay_us(uint64_t *delay_us) {
    if (!g_dev || !delay_us) return -1;
    int bytes = (int)SDL_GetQueuedAudioSize(g_dev);
    int bytes_per_sec = g_sample_rate * g_channels * 2; /* S16 = 2 bytes */
    if (bytes_per_sec <= 0) return -1;
    *delay_us = (uint64_t)bytes * 1000000ULL / (uint64_t)bytes_per_sec;
    return 0;
}

static int g_sw_volume = 80;
static int sdl_set_volume(int vol) { g_sw_volume = vol; return 0; }
static int sdl_get_volume(void)    { return g_sw_volume; }

AudioOutputBackend g_sdl_backend = {
    .name            = "sdl",
    .probe           = sdl_probe,
    .init            = sdl_init,
    .shutdown        = sdl_shutdown,
    .write           = sdl_write_blocking,
    .write_nonblock  = NULL,
    .start           = sdl_start,
    .stop            = sdl_stop,
    .pause           = sdl_pause,
    .resume          = sdl_resume,
    .set_volume      = sdl_set_volume,
    .get_volume      = sdl_get_volume,
    .get_delay_us    = sdl_get_delay_us,
    .flush           = sdl_flush,
};
