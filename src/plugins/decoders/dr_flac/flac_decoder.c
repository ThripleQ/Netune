#include "core/decoder.h"
#include <stdlib.h>
#include <string.h>

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

typedef struct {
    drflac    *flac;
    int       sample_rate;
    int       channels;
    int       total_frames;
} FlacHandle;

static void* flac_open(const char *path) {
    FlacHandle *h = (FlacHandle*)calloc(1, sizeof(FlacHandle));
    if (!h) return NULL;
    h->flac = drflac_open_file(path, NULL);
    if (!h->flac) { free(h); return NULL; }
    h->sample_rate   = (int)h->flac->sampleRate;
    h->channels      = h->flac->channels;
    drflac_uint64 total   = h->flac->totalPCMFrameCount;
    h->total_frames  = (int)total;
    return h;
}

static void* flac_open_mem(const uint8_t *data, size_t size) {
    (void)data; (void)size;
    return NULL;
}

static int flac_get_info(void *handle, int *sample_rate,
                          int *channels, int *total_frames) {
    if (!handle) return -1;
    FlacHandle *h = (FlacHandle*)handle;
    if (sample_rate)  *sample_rate  = h->sample_rate;
    if (channels)     *channels     = h->channels;
    if (total_frames) *total_frames = h->total_frames;
    return 0;
}

static int flac_decode(void *handle, DecodedFrame *frame) {
    if (!handle || !frame) return -1;
    FlacHandle *h = (FlacHandle*)handle;
    frame->sample_rate = h->sample_rate;
    frame->channels    = h->channels;
    int max_frames = 4096;
    frame->data = (int16_t*)malloc(
        (size_t)max_frames * h->channels * sizeof(int16_t));
    if (!frame->data) return -1;
    frame->frames = (int)drflac_read_pcm_frames_s16(
        h->flac, (size_t)max_frames, frame->data);
    if (frame->frames <= 0) {
        free(frame->data);
        frame->data = NULL;
    }
    return frame->frames;
}

static int flac_seek(void *handle, int frame) {
    if (!handle) return -1;
    FlacHandle *h = (FlacHandle*)handle;
    return drflac_seek_to_pcm_frame(h->flac, (drflac_uint64)frame) ? 0 : -1;
}

static void flac_close(void *handle) {
    if (!handle) return;
    FlacHandle *h = (FlacHandle*)handle;
    drflac_close(h->flac);
    free(h);
}

DecoderPlugin g_flac_plugin = {
    .name      = "dr_flac",
    .ext       = "flac",
    .open      = flac_open,
    .open_mem  = flac_open_mem,
    .get_info  = flac_get_info,
    .decode    = flac_decode,
    .seek      = flac_seek,
    .close     = flac_close,
};
