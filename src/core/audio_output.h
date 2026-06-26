#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ── Audio configuration ─────────────────────────────── */
typedef struct {
    int sample_rate;     /* e.g. 44100, 48000            */
    int channels;        /* 1 = mono, 2 = stereo          */
    int bits_per_sample; /* 16, 24, 32                    */
    int buffer_frames;   /* hw buffer size hint            */
} AudioConfig;

/* ── Audio output backend interface ──────────────────── */
typedef struct AudioOutputBackend {
    const char *name;    /* "alsa", "pulseaudio", "sdl"   */

    bool (*probe)(void);
    int  (*init)(const AudioConfig *cfg);
    void (*shutdown)(void);

    /* write PCM: blocking */
    int  (*write)(const uint8_t *data, size_t bytes);
    /* write PCM: non-blocking (returns bytes written or -1) */
    int  (*write_nonblock)(const uint8_t *data, size_t bytes);

    int  (*start)(void);
    int  (*stop)(void);
    int  (*pause)(void);
    int  (*resume)(void);

    /* volume 0-100 */
    int  (*set_volume)(int vol);
    int  (*get_volume)(void);

    /* get precise delay in microseconds for a/v sync */
    int  (*get_delay_us)(uint64_t *delay_us);
} AudioOutputBackend;

#ifdef __cplusplus
}
#endif
