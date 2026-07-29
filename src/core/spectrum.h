#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ── Constants ──────────────────────────────────────── */
#define SPECTRUM_FFT_SIZE  1024  /* FFT window (must be power of 2) */
#define SPECTRUM_BANDS     128   /* output frequency bands */

/* ── Process PCM samples into frequency band magnitudes
 *
 * samples:   SPECTRUM_FFT_SIZE mono int16_t samples (caller should mix
 *            stereo to mono before calling)
 * bands:     output array of SPECTRUM_BANDS floats, each 0.0 ~ 1.0
 * band_count: must equal SPECTRUM_BANDS
 *
 * Applies Hann window, runs real FFT, computes magnitudes,
 * merges bins into logarithmically-spaced bands, normalizes. */
void spectrum_process(const int16_t *samples, float *bands, int band_count);

/* ── Thread-safe shared spectrum buffer
 *
 * Playback thread calls spectrum_set_latest() after FFT + processing.
 * UI thread calls spectrum_get_latest() during render.
 * Avoids event pub/sub overhead (~86 events/sec). */
void spectrum_set_latest(const float *bands);
void spectrum_get_latest(float *out);

#ifdef __cplusplus
}
#endif
