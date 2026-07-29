#include "spectrum.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* ── Pre-computed bit-reversal table for 1024 points ── */
static int s_bitrev[SPECTRUM_FFT_SIZE];
static float s_hann[SPECTRUM_FFT_SIZE];
static float s_twiddle_re[SPECTRUM_FFT_SIZE / 2];
static float s_twiddle_im[SPECTRUM_FFT_SIZE / 2];
static int   s_inited = 0;

/* ── Band boundary table (quadratic spacing) ───────── */
static int s_band_start[SPECTRUM_BANDS + 1];

static void init_tables(void) {
    if (s_inited) return;
    int n = SPECTRUM_FFT_SIZE;
    int log2n = 0, t = n;
    while (t >>= 1) log2n++;

    /* Bit-reversal */
    for (int i = 0; i < n; i++) {
        int rev = 0, x = i;
        for (int j = 0; j < log2n; j++) {
            rev = (rev << 1) | (x & 1);
            x >>= 1;
        }
        s_bitrev[i] = rev;
    }

    /* Hann window */
    for (int i = 0; i < n; i++) {
        s_hann[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (n - 1)));
    }

    /* Twiddle factors (for N-point FFT, need N/2 factors) */
    for (int i = 0; i < n / 2; i++) {
        float ang = -2.0f * M_PI * i / n;
        s_twiddle_re[i] = cosf(ang);
        s_twiddle_im[i] = sinf(ang);
    }

    /* Band boundaries: hybrid mapping.
       Bands 0-24 each cover exactly 1 FFT bin (~43 Hz each).
       This gives high resolution in the low end where music
       has the most harmonic detail.
       Bands 25-127 use ^2.5 progressive spacing over the
       remaining frequency range up to ~15 kHz. */
    int sample_rate = 44100;
    int min_bin = (int)(40.0f * SPECTRUM_FFT_SIZE / sample_rate + 0.5f);
    int max_bin = (int)(15000.0f * SPECTRUM_FFT_SIZE / sample_rate + 0.5f);
    if (min_bin < 1) min_bin = 1;
    if (max_bin > SPECTRUM_FFT_SIZE / 2) max_bin = SPECTRUM_FFT_SIZE / 2;

    int tight_bands = 25;  /* bands 0-24 inclusive */
    int tight_bins  = tight_bands;  /* 1 bin each */

    /* Ensure monotonic: each band advances at least 1 bin */
    int prev_start = min_bin - 1;
    for (int i = 0; i <= SPECTRUM_BANDS; i++) {
        int start;
        if (i <= tight_bands) {
            start = min_bin + i;
        } else {
            int remain_bands = SPECTRUM_BANDS - tight_bands;
            int remain_bins  = max_bin - (min_bin + tight_bins);
            float sub = (float)(i - tight_bands) / remain_bands;
            start = (min_bin + tight_bins)
                  + (int)(powf(sub, 2.5f) * remain_bins + 0.5f);
        }
        if (start > max_bin) start = max_bin;
        if (start <= prev_start) start = prev_start + 1;
        s_band_start[i] = start;
        prev_start = start;
    }
    s_band_start[SPECTRUM_BANDS] = max_bin;

    s_inited = 1;
}

/* ── In-place iterative radix-2 FFT ────────────────── */
static void fft(float *re, float *im, int n) {
    /* Bit-reversal permutation */
    for (int i = 0; i < n; i++) {
        int j = s_bitrev[i];
        if (j > i) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }

    /* Butterfly stages */
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1;
        int step = n / len;  /* step in twiddle table */
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < half; j++) {
                int k = j * step;
                float wre = s_twiddle_re[k];
                float wim = s_twiddle_im[k];

                float ur = re[i + j];
                float ui = im[i + j];
                float vr = re[i + j + half] * wre - im[i + j + half] * wim;
                float vi = re[i + j + half] * wim + im[i + j + half] * wre;

                re[i + j]           = ur + vr;
                im[i + j]           = ui + vi;
                re[i + j + half]    = ur - vr;
                im[i + j + half]    = ui - vi;
            }
        }
    }
}

/* ── Public API ─────────────────────────────────────── */
void spectrum_process(const int16_t *samples, float *bands, int band_count) {
    if (!samples || !bands || band_count != SPECTRUM_BANDS) return;

    init_tables();

    int n = SPECTRUM_FFT_SIZE;
    static float re[SPECTRUM_FFT_SIZE];
    static float im[SPECTRUM_FFT_SIZE];

    /* Apply Hann window, pack into complex array (imag = 0) */
    for (int i = 0; i < n; i++) {
        re[i] = samples[i] * s_hann[i];
        im[i] = 0.0f;
    }

    /* FFT */
    fft(re, im, n);

    /* Compute magnitudes for meaningful bins (0..n/2) */
    int half = n / 2;
    float mags[SPECTRUM_FFT_SIZE / 2 + 1];
    for (int i = 0; i <= half; i++) {
        mags[i] = sqrtf(re[i] * re[i] + im[i] * im[i]);
    }

    /* Normalization factor: scale so a full-scale sine at a bin
       produces ~1.0.  For a sine amplitude A at bin k:
         |X[k]| ≈ A * n / 4  (Hann window, single-sided)
       So norm = 1 / (32767 * n / 4) = 4 / (32767 * n). */
    float norm = 4.0f / (32767.0f * n);

    /* Merge bins into bands (quadratic spacing) */
    for (int b = 0; b < band_count; b++) {
        int start = s_band_start[b];
        int end   = s_band_start[b + 1];
        int count = end - start;
        if (count <= 0) {
            bands[b] = 0.0f;
            continue;
        }
        double sum = 0.0;
        for (int i = start; i < end; i++) {
            sum += mags[i];
        }
        float avg = (float)(sum / count) * norm;
        /* Clamp to [0, 1] */
        if (avg > 1.0f) avg = 1.0f;
        if (avg < 0.0f) avg = 0.0f;
        bands[b] = avg;
    }
}

/* ── Thread-safe shared spectrum buffer ────────────── */
static float s_spectrum_latest[SPECTRUM_BANDS];

void spectrum_set_latest(const float *bands) {
    memcpy(s_spectrum_latest, bands, sizeof(s_spectrum_latest));
}

void spectrum_get_latest(float *out) {
    memcpy(out, s_spectrum_latest, sizeof(s_spectrum_latest));
}
