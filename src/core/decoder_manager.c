#include "decoder_manager.h"
#include "infra/log.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DR_MP3_IMPLEMENTATION
#include "plugins/decoders/dr_libs/dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#include "plugins/decoders/dr_libs/dr_flac.h"

#define DR_WAV_IMPLEMENTATION
#include "plugins/decoders/dr_libs/dr_wav.h"

/* ── Supported extensions ──────────────────────────── */
static const char *g_supported[] = {"mp3", "flac", "wav", NULL};

bool decoder_supports_ext(const char *ext) {
    if (!ext) return false;
    for (int i = 0; g_supported[i]; i++) {
        if (strcasecmp(ext, g_supported[i]) == 0) return true;
    }
    return false;
}

/* ── Backend type ──────────────────────────────────── */
typedef enum { BACKEND_MP3, BACKEND_FLAC, BACKEND_WAV } BackendType;

struct Decoder {
    BackendType type;
    union {
        drmp3  mp3;
        drflac *flac;
        drwav  wav;
    } u;
    DecoderInfo info;
    bool opened;
};

static BackendType detect_backend(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return BACKEND_WAV; /* fallback */
    const char *ext = dot + 1;
    if (strcasecmp(ext, "mp3") == 0)  return BACKEND_MP3;
    if (strcasecmp(ext, "flac") == 0) return BACKEND_FLAC;
    return BACKEND_WAV;
}

/* ── Open ──────────────────────────────────────────── */
Decoder* decoder_open(const char *path) {
    if (!path) return NULL;

    Decoder *d = (Decoder*)calloc(1, sizeof(Decoder));
    if (!d) return NULL;

    d->type = detect_backend(path);

    switch (d->type) {
    case BACKEND_MP3: {
        if (!drmp3_init_file(&d->u.mp3, path, NULL)) {
            LOG_ERROR("drmp3_init_file failed: %s", path);
            free(d);
            return NULL;
        }
        d->info.samplerate   = d->u.mp3.sampleRate;
        d->info.channels     = d->u.mp3.channels;
        /* dr_mp3 provides totalPcmFrameCount */
        drmp3_uint64 total = drmp3_get_pcm_frame_count(&d->u.mp3);
        d->info.total_frames = (total > 0) ? (int)total : -1;
        break;
    }
    case BACKEND_FLAC: {
        d->u.flac = drflac_open_file(path, NULL);
        if (!d->u.flac) {
            LOG_ERROR("drflac_open_file failed: %s", path);
            free(d);
            return NULL;
        }
        d->info.samplerate   = (int)d->u.flac->sampleRate;
        d->info.channels     = d->u.flac->channels;
        drflac_uint64 total = d->u.flac->totalPCMFrameCount;
        d->info.total_frames = (total > 0) ? (int)total : -1;
        break;
    }
    case BACKEND_WAV: {
        if (!drwav_init_file(&d->u.wav, path, NULL)) {
            LOG_ERROR("drwav_init_file failed: %s", path);
            free(d);
            return NULL;
        }
        d->info.samplerate   = d->u.wav.sampleRate;
        d->info.channels     = d->u.wav.channels;
        d->info.total_frames = (int)d->u.wav.totalPCMFrameCount;
        break;
    }
    }

    d->opened = true;
    LOG_INFO("Decoder opened: %s  (%dHz %dch %dframes)",
             path, d->info.samplerate, d->info.channels, d->info.total_frames);
    return d;
}

int decoder_get_info(const Decoder *d, DecoderInfo *info) {
    if (!d || !info) return -1;
    *info = d->info;
    return 0;
}

int decoder_decode(Decoder *d, int16_t *pcm, int max_frames) {
    if (!d || !pcm || max_frames <= 0) return -1;

    switch (d->type) {
    case BACKEND_MP3:
        return (int)drmp3_read_pcm_frames_s16(&d->u.mp3, (size_t)max_frames, pcm);
    case BACKEND_FLAC:
        return (int)drflac_read_pcm_frames_s16(d->u.flac, (size_t)max_frames, pcm);
    case BACKEND_WAV:
        return (int)drwav_read_pcm_frames_s16(&d->u.wav, (size_t)max_frames, pcm);
    }
    return -1;
}

int decoder_seek(Decoder *d, int frame) {
    if (!d || frame < 0) return -1;

    switch (d->type) {
    case BACKEND_MP3:
        return drmp3_seek_to_pcm_frame(&d->u.mp3, (drmp3_uint64)frame) ? 0 : -1;
    case BACKEND_FLAC:
        return drflac_seek_to_pcm_frame(d->u.flac, (drflac_uint64)frame) ? 0 : -1;
    case BACKEND_WAV:
        return drwav_seek_to_pcm_frame(&d->u.wav, (drwav_uint64)frame) ? 0 : -1;
    }
    return -1;
}

void decoder_close(Decoder *d) {
    if (!d) return;
    if (d->opened) {
        switch (d->type) {
        case BACKEND_MP3: drmp3_uninit(&d->u.mp3); break;
        case BACKEND_FLAC: drflac_close(d->u.flac); break;
        case BACKEND_WAV: drwav_uninit(&d->u.wav); break;
        }
    }
    free(d);
}
