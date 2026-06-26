#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

typedef struct AudioOutput AudioOutput;

/* Create audio output with given format.
   Uses ALSA by default, falls back if unavailable. */
AudioOutput* audio_output_create(int sample_rate, int channels);

/* Destroy and close */
void audio_output_destroy(AudioOutput *ao);

/* Write interleaved s16 PCM. Blocking. Returns frames written or <0 */
int audio_output_write(AudioOutput *ao, const int16_t *pcm, int frames);

/* Get the current playback delay in microseconds (for sync) */
int audio_output_delay_us(AudioOutput *ao, uint64_t *delay_us);

#ifdef __cplusplus
}
#endif
