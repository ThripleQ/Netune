#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ── Constants ──────────────────────────────────────── */
#define SPECTRUM_FFT_SIZE  2048  /* FFT window (must be power of 2) */
#define SPECTRUM_BANDS     (SPECTRUM_FFT_SIZE / 2)  /* one FFT bin per band */

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

#ifdef __cplusplus
}
#endif
