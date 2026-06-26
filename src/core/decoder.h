#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ── Decoded frame ────────────────────────────────────── */
typedef struct {
    int16_t *data;       /* interleaved PCM samples       */
    int      frames;     /* frames decoded this call       */
    int      sample_rate;
    int      channels;
} DecodedFrame;

/* ── Decoder plugin interface ──────────────────────────── */
typedef struct DecoderPlugin {
    const char *name;
    const char *ext;      /* file extension: "mp3","flac","ogg" */

    /* open file, return handle (opaque) */
    void* (*open)(const char *path);
    /* open from memory */
    void* (*open_mem)(const uint8_t *data, size_t size);

    /* get format info (sample_rate, channels, total_frames) */
    int   (*get_info)(void *handle, int *sample_rate,
                      int *channels, int *total_frames);

    /* decode next chunk -> DecodedFrame (caller frees frame.data) */
    int   (*decode)(void *handle, DecodedFrame *frame);

    /* seek to frame position */
    int   (*seek)(void *handle, int frame);

    /* close */
    void  (*close)(void *handle);
} DecoderPlugin;

#ifdef __cplusplus
}
#endif
