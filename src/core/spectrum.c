#include "spectrum.h"
#include <math.h>

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

    /* Hybrid: low end linear (1 bin each), then logarithmic.
       FFT 2048 @ 44100 Hz → ~21.5 Hz/bin.
       Bands 0-24: linear, bins 1-26 (25 bands, 22-560 Hz)
       Bands 25-127: log spacing, bin 26 ~ 20000 Hz */
    int sample_rate = 44100;
    int linear = 25;  /* bands 0-24 */
    int max_bin = (int)(20000.0 * SPECTRUM_FFT_SIZE / sample_rate);
    if (max_bin > SPECTRUM_FFT_SIZE / 2) max_bin = SPECTRUM_FFT_SIZE / 2;

    /* Linear section: one bin per band */
    for (int i = 0; i <= linear; i++)
        s_band_start[i] = 1 + i;

    /* Logarithmic section from bin 26 upward */
    int log_bands = SPECTRUM_BANDS - linear;  /* 103 bands */
    double f_start = (double)s_band_start[linear] * sample_rate / SPECTRUM_FFT_SIZE;
    double log_step = log(20000.0 / f_start) / log_bands;
    for (int i = 0; i <= log_bands; i++) {
        double f = f_start * exp(i * log_step);
        int bin = (int)(f * SPECTRUM_FFT_SIZE / sample_rate);
        if (bin > max_bin) bin = max_bin;
        /* Ensure at least 1 bin from previous */
        int prev = s_band_start[linear + i - 1];
        if (i > 0 && bin <= prev) bin = prev + 1;
        if (bin < s_band_start[linear]) bin = s_band_start[linear];
        s_band_start[linear + i] = bin;
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
