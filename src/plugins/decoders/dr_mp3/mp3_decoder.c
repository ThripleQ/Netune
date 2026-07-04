#include "core/decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

/* ── Custom I/O for FIFO/pipe support ─────────────────
   drmp3_init_file() uses fseek/fseek which fail on pipes.
   Use drmp3_init_internal() with custom callbacks instead. */

typedef struct {
    FILE   *fp;
    drmp3_int64 bytes_read;   /* rough position for onTell */
} StreamIO;

static size_t stream_read(void *pUserData, void *pBufferOut, size_t bytesToRead) {
    StreamIO *io = (StreamIO*)pUserData;
    size_t n = fread(pBufferOut, 1, bytesToRead, io->fp);
    io->bytes_read += (drmp3_int64)n;
    return n;
}

static drmp3_bool32 stream_seek(void *pUserData, int offset, drmp3_seek_origin origin) {
    StreamIO *io = (StreamIO*)pUserData;
    /* For pipes, we can only seek to the start (position 0).
       SEEK_END and SEEK_CUR will fail gracefully — dr_mp3 handles this
       by skipping ID3 tag detection and using DRMP3_UINT64_MAX for length. */
    if (origin == DRMP3_SEEK_SET && offset == 0) {
        /* Can't rewind a pipe, but dr_mp3 only calls SEEK_SET|0
           after a failed SEEK_END to "reset" the cursor.
           For a pipe this is a no-op; the cursor is already at 0. */
        io->bytes_read = 0;
        return DRMP3_TRUE;
    }
    return DRMP3_FALSE;  /* SEEK_END, SEEK_CUR, or non-zero SEEK_SET all fail */
}

static drmp3_bool32 stream_tell(void *pUserData, drmp3_int64 *pCursor) {
    StreamIO *io = (StreamIO*)pUserData;
    if (pCursor) *pCursor = io->bytes_read;
    return DRMP3_TRUE;
}

/* ── Plugin handle ─────────────────────────────────── */
typedef struct {
    drmp3      mp3;
    StreamIO   io;
    int        sample_rate;
    int        channels;
    int        total_frames;
} Mp3Handle;

static void* mp3_open(const char *path) {
    Mp3Handle *h = (Mp3Handle*)calloc(1, sizeof(Mp3Handle));
    if (!h) return NULL;

    h->io.fp = fopen(path, "rb");
    if (!h->io.fp) { free(h); return NULL; }

    drmp3_bool32 ok = drmp3_init_internal(&h->mp3,
        stream_read, stream_seek, stream_tell,
        NULL,                     /* onMeta */
        &h->io,                   /* pUserData */
        NULL,                     /* pUserDataMeta */
        NULL);                    /* pAllocationCallbacks (defaults) */

    if (!ok) {
        fclose(h->io.fp);
        free(h);
        return NULL;
    }

    h->sample_rate  = h->mp3.sampleRate;
    h->channels     = h->mp3.channels;

    /* total frames: will fail on pipes (returns 0) — set to -1 */
    drmp3_uint64 total = drmp3_get_pcm_frame_count(&h->mp3);
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
    if (h->total_frames < 0) return -1;  /* pipe: no seeking */
    return drmp3_seek_to_pcm_frame(&h->mp3, (drmp3_uint64)frame) ? 0 : -1;
}

static void mp3_close(void *handle) {
    if (!handle) return;
    Mp3Handle *h = (Mp3Handle*)handle;
    drmp3_uninit(&h->mp3);
    fclose(h->io.fp);
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
