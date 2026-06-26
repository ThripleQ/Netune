#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "audio_output.h"  /* AudioOutputBackend, AudioConfig */

/* ── Opaque output handle ──────────────────────────── */
typedef struct AudioOutput AudioOutput;

/* ── Plugin registration ──────────────────────────── */
int audio_output_register_backend(AudioOutputBackend *backend);

/* ── High-level API ────────────────────────────────── */
AudioOutput* audio_output_create(int sample_rate, int channels);
void         audio_output_destroy(AudioOutput *ao);
int          audio_output_write(AudioOutput *ao, const int16_t *pcm, int frames);
int          audio_output_delay_us(AudioOutput *ao, uint64_t *delay_us);
int          audio_output_flush(AudioOutput *ao);
int          audio_output_set_volume(int vol);  /* 0-100 */
int          audio_output_get_volume(void);       /* -1 if unsupported */

#ifdef __cplusplus
}
#endif
