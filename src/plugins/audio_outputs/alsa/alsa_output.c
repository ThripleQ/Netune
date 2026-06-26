#include "core/audio_output_mgr.h"
#include "infra/log.h"
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>

struct AudioOutput {
    snd_pcm_t          *pcm;
    int                 sample_rate;
    int                 channels;
    snd_pcm_uframes_t   period_size;
    int                 bytes_per_frame;
};

AudioOutput* audio_output_create(int sample_rate, int channels) {
    AudioOutput *ao = (AudioOutput*)calloc(1, sizeof(AudioOutput));
    if (!ao) return NULL;

    ao->sample_rate = sample_rate;
    ao->channels    = channels;
    ao->bytes_per_frame = channels * 2; /* s16 = 2 bytes */
    ao->period_size = 4096;

    int rc;
    snd_pcm_hw_params_t *hw = NULL;

    rc = snd_pcm_open(&ao->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        LOG_ERROR("ALSA open failed: %s", snd_strerror(rc));
        free(ao);
        return NULL;
    }

    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(ao->pcm, hw);
    snd_pcm_hw_params_set_access(ao->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(ao->pcm, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_rate_near(ao->pcm, hw, (unsigned int*)&sample_rate, NULL);
    snd_pcm_hw_params_set_channels_near(ao->pcm, hw, (unsigned int*)&channels);
    snd_pcm_uframes_t buf_frames = ao->period_size * 4;
    snd_pcm_hw_params_set_buffer_size_near(ao->pcm, hw, &buf_frames);
    snd_pcm_hw_params_set_period_size_near(ao->pcm, hw, &ao->period_size, NULL);

    rc = snd_pcm_hw_params(ao->pcm, hw);
    if (rc < 0) {
        LOG_ERROR("ALSA hw_params failed: %s", snd_strerror(rc));
        snd_pcm_close(ao->pcm);
        free(ao);
        return NULL;
    }

    snd_pcm_prepare(ao->pcm);

    LOG_INFO("ALSA output: %dHz %dch, period=%lu", sample_rate, channels, ao->period_size);
    return ao;
}

void audio_output_destroy(AudioOutput *ao) {
    if (!ao) return;
    if (ao->pcm) {
        snd_pcm_drain(ao->pcm);
        snd_pcm_close(ao->pcm);
    }
    free(ao);
    LOG_DEBUG("ALSA output destroyed");
}

int audio_output_write(AudioOutput *ao, const int16_t *pcm, int frames) {
    if (!ao || !ao->pcm) return -1;

    /* xrun recovery */
    int rc = snd_pcm_writei(ao->pcm, pcm, (snd_pcm_uframes_t)frames);
    if (rc == -EPIPE) {
        snd_pcm_prepare(ao->pcm);
        LOG_WARN("ALSA xrun (underrun), recovered");
        rc = snd_pcm_writei(ao->pcm, pcm, (snd_pcm_uframes_t)frames);
    } else if (rc == -ESTRPIPE) {
        while ((rc = snd_pcm_resume(ao->pcm)) == -EAGAIN)
            ; /* wait for resume */
        rc = snd_pcm_writei(ao->pcm, pcm, (snd_pcm_uframes_t)frames);
    }

    if (rc < 0) {
        LOG_ERROR("ALSA write error: %s", snd_strerror(rc));
        return rc;
    }

    return rc; /* frames actually written */
}

int audio_output_delay_us(AudioOutput *ao, uint64_t *delay_us) {
    if (!ao || !ao->pcm) return -1;

    snd_pcm_sframes_t delay;
    int rc = snd_pcm_delay(ao->pcm, &delay);
    if (rc < 0) return -1;

    *delay_us = (uint64_t)delay * 1000000 / ao->sample_rate;
    return 0;
}
