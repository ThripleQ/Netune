#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "decoder.h"  /* for DecoderPlugin, DecodedFrame */

/* ── Decoder info ──────────────────────────────────── */
typedef struct {
    int sample_rate;
    int channels;
    int total_frames;   /* -1 = unknown */
} DecoderInfo;

/* ── Opaque decoder handle ────────────────────────── */
typedef struct Decoder Decoder;

/* ── Plugin registration ──────────────────────────── */
int decoder_register_plugin(DecoderPlugin *plugin);

/* ── High-level API ────────────────────────────────── */
Decoder* decoder_open(const char *path);
int  decoder_get_info(const Decoder *d, DecoderInfo *info);
int  decoder_decode(Decoder *d, int16_t *pcm, int max_frames);
int  decoder_seek(Decoder *d, int frame);
void decoder_close(Decoder *d);
bool decoder_supports_ext(const char *ext);

#ifdef __cplusplus
}
#endif
