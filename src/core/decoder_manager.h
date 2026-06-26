#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Decoder info ──────────────────────────────────── */
typedef struct {
    int samplerate;
    int channels;
    int total_frames;   /* -1 = unknown */
} DecoderInfo;

/* ── Opaque decoder handle ────────────────────────── */
typedef struct Decoder Decoder;

/* ── API ──────────────────────────────────────────── */
/* Open by file path (auto-detects format from extension) */
Decoder* decoder_open(const char *path);

/* Get format info */
int decoder_get_info(const Decoder *d, DecoderInfo *info);

/* Decode next chunk into interleaved int16 PCM.
   Returns number of frames decoded, 0 = EOF, <0 = error. */
int decoder_decode(Decoder *d, int16_t *pcm, int max_frames);

/* Seek to frame position (0-based). Returns 0 on success. */
int decoder_seek(Decoder *d, int frame);

/* Close and free */
void decoder_close(Decoder *d);

/* Check if file extension is supported */
bool decoder_supports_ext(const char *ext);

#ifdef __cplusplus
}
#endif
