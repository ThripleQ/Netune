#include "core/audio_output.h"
#include "infra/log.h"
#include <stdlib.h>
#include <alsa/asoundlib.h>

static snd_pcm_t *g_pcm = NULL;
static int g_sample_rate = 44100;
static int g_channels = 2;

/* mixer */
static snd_mixer_t      *g_mixer = NULL;
static snd_mixer_elem_t *g_mixer_elem = NULL;
static long g_mixer_min = 0, g_mixer_max = 100;

static int mixer_init(void) {
    if (g_mixer) return 0;
    if (snd_mixer_open(&g_mixer, 0) < 0) return -1;
    if (snd_mixer_attach(g_mixer, "default") < 0) { snd_mixer_close(g_mixer); g_mixer = NULL; return -1; }
    if (snd_mixer_selem_register(g_mixer, NULL, NULL) < 0) { snd_mixer_close(g_mixer); g_mixer = NULL; return -1; }
    if (snd_mixer_load(g_mixer) < 0) { snd_mixer_close(g_mixer); g_mixer = NULL; return -1; }

    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "Master");
    g_mixer_elem = snd_mixer_find_selem(g_mixer, sid);
    if (!g_mixer_elem) {
        snd_mixer_selem_id_set_name(sid, "PCM");
        g_mixer_elem = snd_mixer_find_selem(g_mixer, sid);
    }
    if (!g_mixer_elem) {
        LOG_WARN("No Master/PCM mixer element found");
        snd_mixer_close(g_mixer);
        g_mixer = NULL;
        return -1;
    }
    snd_mixer_selem_get_playback_volume_range(g_mixer_elem, &g_mixer_min, &g_mixer_max);
    LOG_INFO("Mixer found: %s  range=[%ld,%ld]",
             snd_mixer_selem_id_get_name(sid), g_mixer_min, g_mixer_max);
    return 0;
}

static bool alsa_probe(void) {
    snd_pcm_t *test = NULL;
    int rc = snd_pcm_open(&test, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (test) snd_pcm_close(test);
    return rc == 0;
}

static int alsa_init(const AudioConfig *cfg) {
    if (!cfg) return -1;
    int rc;
    snd_pcm_hw_params_t *hw = NULL;
    rc = snd_pcm_open(&g_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) { LOG_ERROR("ALSA open failed: %s", snd_strerror(rc)); return -1; }

    g_sample_rate = cfg->sample_rate;
    g_channels    = cfg->channels;

    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(g_pcm, hw);
    snd_pcm_hw_params_set_access(g_pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(g_pcm, hw, SND_PCM_FORMAT_S16_LE);

    unsigned int rate = (unsigned int)cfg->sample_rate;
    unsigned int ch   = (unsigned int)cfg->channels;
    snd_pcm_hw_params_set_rate_near(g_pcm, hw, &rate, NULL);
    snd_pcm_hw_params_set_channels_near(g_pcm, hw, &ch);

    snd_pcm_uframes_t period_frames = 4096;
    snd_pcm_uframes_t buf_frames = period_frames * 4;
    snd_pcm_hw_params_set_buffer_size_near(g_pcm, hw, &buf_frames);
    snd_pcm_hw_params_set_period_size_near(g_pcm, hw, &period_frames, NULL);

    rc = snd_pcm_hw_params(g_pcm, hw);
    if (rc < 0) { LOG_ERROR("ALSA hw_params failed: %s", snd_strerror(rc)); snd_pcm_close(g_pcm); g_pcm = NULL; return -1; }

    snd_pcm_prepare(g_pcm);
    g_sample_rate = (int)rate;
    g_channels    = (int)ch;
    LOG_INFO("ALSA initialized: %dHz %dch", g_sample_rate, g_channels);
    return 0;
}

static void alsa_shutdown(void) {
    if (g_pcm) { snd_pcm_drain(g_pcm); snd_pcm_close(g_pcm); g_pcm = NULL; }
    if (g_mixer) { snd_mixer_close(g_mixer); g_mixer = NULL; g_mixer_elem = NULL; }
}

static int alsa_write(const uint8_t *data, size_t bytes) {
    if (!g_pcm) return -1;
    snd_pcm_uframes_t frames = (snd_pcm_uframes_t)(bytes / (g_channels * 2));
    int rc = snd_pcm_writei(g_pcm, data, frames);
    if (rc == -EPIPE) { snd_pcm_prepare(g_pcm); rc = (int)snd_pcm_writei(g_pcm, data, frames); }
    else if (rc == -ESTRPIPE) { while (snd_pcm_resume(g_pcm) == -EAGAIN); rc = (int)snd_pcm_writei(g_pcm, data, frames); }
    if (rc < 0) { LOG_ERROR("ALSA write error: %s", snd_strerror(rc)); return rc; }
    return rc;
}

static int alsa_start(void) { return g_pcm ? (snd_pcm_start(g_pcm) == 0 ? 0 : -1) : -1; }
static int alsa_stop(void)  { return g_pcm ? (snd_pcm_drop(g_pcm) == 0 ? 0 : -1) : -1; }
static int alsa_pause(void) { return g_pcm ? (snd_pcm_pause(g_pcm, 1) == 0 ? 0 : -1) : -1; }
static int alsa_resume(void) { return g_pcm ? (snd_pcm_pause(g_pcm, 0) == 0 ? 0 : -1) : -1; }

static int alsa_set_volume(int vol) {
    if (mixer_init() < 0 || !g_mixer_elem) return -1;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    long hw_vol = g_mixer_min + (long)((double)vol / 100.0 * (g_mixer_max - g_mixer_min));
    snd_mixer_selem_set_playback_volume_all(g_mixer_elem, hw_vol);
    LOG_INFO("Volume set to %d (hw: %ld)", vol, hw_vol);
    return 0;
}

static int alsa_get_volume(void) {
    if (mixer_init() < 0 || !g_mixer_elem) return -1;
    long hw_vol;
    if (snd_mixer_selem_get_playback_volume(g_mixer_elem, SND_MIXER_SCHN_MONO, &hw_vol) < 0)
        return -1;
    int vol = (int)((double)(hw_vol - g_mixer_min) / (g_mixer_max - g_mixer_min) * 100.0);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    return vol;
}

static int alsa_get_delay_us(uint64_t *delay_us) {
    if (!g_pcm || !delay_us) return -1;
    snd_pcm_sframes_t delay;
    if (snd_pcm_delay(g_pcm, &delay) < 0) return -1;
    *delay_us = (uint64_t)delay * 1000000 / g_sample_rate;
    return 0;
}

static int alsa_flush(void) {
    if (!g_pcm) return -1;
    snd_pcm_drop(g_pcm);
    snd_pcm_prepare(g_pcm);
    return 0;
}

AudioOutputBackend g_alsa_backend = {
    .name           = "alsa",
    .probe          = alsa_probe,
    .init           = alsa_init,
    .shutdown       = alsa_shutdown,
    .write          = alsa_write,
    .write_nonblock = NULL,
    .start          = alsa_start,
    .stop           = alsa_stop,
    .pause          = alsa_pause,
    .resume         = alsa_resume,
    .set_volume     = alsa_set_volume,
    .get_volume     = alsa_get_volume,
    .get_delay_us   = alsa_get_delay_us,
    .flush          = alsa_flush,
};
