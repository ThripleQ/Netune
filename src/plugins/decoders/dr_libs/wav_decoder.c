#include "core/decoder.h"
#include <stdlib.h>
#include <string.h>

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

typedef struct {
    drwav     wav;
    int       sample_rate;
    int       channels;
    int       total_frames;
} WavHandle;

static void* wav_open(const char *path) {
    WavHandle *h = (WavHandle*)calloc(1, sizeof(WavHandle));
    if (!h) return NULL;
    if (!drwav_init_file(&h->wav, path, NULL)) {
        free(h);
        return NULL;
    }
    h->sample_rate   = h->wav.sampleRate;
    h->channels      = h->wav.channels;
    h->total_frames  = (int)h->wav.totalPCMFrameCount;
    return h;
}

static void* wav_open_mem(const uint8_t *data, size_t size) {
    (void)data; (void)size;
    return NULL;
}

static int wav_get_info(void *handle, int *sample_rate,
                         int *channels, int *total_frames) {
    if (!handle) return -1;
    WavHandle *h = (WavHandle*)handle;
    if (sample_rate)  *sample_rate  = h->sample_rate;
    if (channels)     *channels     = h->channels;
    if (total_frames) *total_frames = h->total_frames;
    return 0;
}

static int wav_decode(void *handle, DecodedFrame *frame) {
    if (!handle || !frame) return -1;
    WavHandle *h = (WavHandle*)handle;
    frame->sample_rate = h->sample_rate;
    frame->channels    = h->channels;
    int max_frames = 4096;
    frame->data = (int16_t*)malloc(
        (size_t)max_frames * h->channels * sizeof(int16_t));
    if (!frame->data) return -1;
    frame->frames = (int)drwav_read_pcm_frames_s16(
        &h->wav, (size_t)max_frames, frame->data);
    if (frame->frames <= 0) {
        free(frame->data);
        frame->data = NULL;
    }
    return frame->frames;
}

static int wav_seek(void *handle, int frame) {
    if (!handle) return -1;
    WavHandle *h = (WavHandle*)handle;
    return drwav_seek_to_pcm_frame(&h->wav, (drwav_uint64)frame) ? 0 : -1;
}

static void wav_close(void *handle) {
    if (!handle) return;
    WavHandle *h = (WavHandle*)handle;
    drwav_uninit(&h->wav);
    free(h);
}

DecoderPlugin g_wav_plugin = {
    .name      = "dr_wav",
    .ext       = "wav",
    .open      = wav_open,
    .open_mem  = wav_open_mem,
    .get_info  = wav_get_info,
    .decode    = wav_decode,
    .seek      = wav_seek,
    .close     = wav_close,
};
