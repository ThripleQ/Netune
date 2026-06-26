#include "core/decoder.h"
#include <stdlib.h>
#include <string.h>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

typedef struct {
    drmp3     mp3;
    int       sample_rate;
    int       channels;
    int       total_frames;
} Mp3Handle;

static void* mp3_open(const char *path) {
    Mp3Handle *h = (Mp3Handle*)calloc(1, sizeof(Mp3Handle));
    if (!h) return NULL;
    if (!drmp3_init_file(&h->mp3, path, NULL)) {
        free(h);
        return NULL;
    }
    h->sample_rate   = h->mp3.sampleRate;
    h->channels      = h->mp3.channels;
    drmp3_uint64 total    = drmp3_get_pcm_frame_count(&h->mp3);
    h->total_frames  = (total > 0) ? (int)total : -1;
    return h;
}

static void* mp3_open_mem(const uint8_t *data, size_t size) {
    (void)data; (void)size;
    return NULL; /* not implemented yet */
}

static int mp3_get_info(void *handle, int *sample_rate,
                         int *channels, int *total_frames) {
    if (!handle) return -1;
    Mp3Handle *h = (Mp3Handle*)handle;
    if (sample_rate)  *sample_rate  = h->sample_rate;
    if (channels)     *channels     = h->channels;
    if (total_frames) *total_frames = h->total_frames;
    return 0;
}

static int mp3_decode(void *handle, DecodedFrame *frame) {
    if (!handle || !frame) return -1;
    Mp3Handle *h = (Mp3Handle*)handle;
    frame->sample_rate = h->sample_rate;
    frame->channels    = h->channels;
    /* use a reasonable chunk size */
    int max_frames = 4096;
    frame->data = (int16_t*)malloc(
        (size_t)max_frames * h->channels * sizeof(int16_t));
    if (!frame->data) return -1;
    frame->frames = (int)drmp3_read_pcm_frames_s16(
        &h->mp3, (size_t)max_frames, frame->data);
    if (frame->frames <= 0) {
        free(frame->data);
        frame->data = NULL;
    }
    return frame->frames;
}

static int mp3_seek(void *handle, int frame) {
    if (!handle) return -1;
    Mp3Handle *h = (Mp3Handle*)handle;
    return drmp3_seek_to_pcm_frame(&h->mp3, (drmp3_uint64)frame) ? 0 : -1;
}

static void mp3_close(void *handle) {
    if (!handle) return;
    Mp3Handle *h = (Mp3Handle*)handle;
    drmp3_uninit(&h->mp3);
    free(h);
}

DecoderPlugin g_mp3_plugin = {
    .name      = "dr_mp3",
    .ext       = "mp3",
    .open      = mp3_open,
    .open_mem  = mp3_open_mem,
    .get_info  = mp3_get_info,
    .decode    = mp3_decode,
    .seek      = mp3_seek,
    .close     = mp3_close,
};
