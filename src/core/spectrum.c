#include "spectrum.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* ── Pre-computed tables ────────────────────────────── */
static int   s_bitrev[SPECTRUM_FFT_SIZE];
static float s_hann[SPECTRUM_FFT_SIZE];
static float s_twiddle_re[SPECTRUM_FFT_SIZE / 2];
static float s_twiddle_im[SPECTRUM_FFT_SIZE / 2];
static int   s_inited = 0;

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

    /* Twiddle factors */
    for (int i = 0; i < n / 2; i++) {
        float ang = -2.0f * M_PI * i / n;
        s_twiddle_re[i] = cosf(ang);
        s_twiddle_im[i] = sinf(ang);
    }

    s_inited = 1;
}

/* ── In-place iterative radix-2 FFT ────────────────── */
static void fft(float *re, float *im, int n) {
    for (int i = 0; i < n; i++) {
        int j = s_bitrev[i];
        if (j > i) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1;
        int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < half; j++) {
                int k = j * step;
                float wre = s_twiddle_re[k];
                float wim = s_twiddle_im[k];

                float ur = re[i + j];
                float ui = im[i + j];
                float vr = re[i + j + half] * wre - im[i + j + half] * wim;
                float vi = re[i + j + half] * wim + im[i + j + half] * wre;

                re[i + j]        = ur + vr;
                im[i + j]        = ui + vi;
                re[i + j + half] = ur - vr;
                im[i + j + half] = ui - vi;
            }
        }
    }
}

/* ── Public API ───────────────────────────────────────
   128 linear bands covering 0 ~ mid-range (~8 kHz): each band is a
   plain average of a few consecutive FFT bins — no log spacing, no
   dB, no smoothing. */
void spectrum_process(const int16_t *samples, float *bands, int band_count) {
    if (!samples || !bands || band_count > SPECTRUM_BANDS) return;

    init_tables();

    int n = SPECTRUM_FFT_SIZE;
    static float re[SPECTRUM_FFT_SIZE];
    static float im[SPECTRUM_FFT_SIZE];

    for (int i = 0; i < n; i++) {
        re[i] = samples[i] * s_hann[i];
        im[i] = 0.0f;
    }

    fft(re, im, n);

    /* Normalization: full-scale sine → ~1.0 */
    float norm = 4.0f / (32767.0f * n);

    /* Mid-range coverage: bands span bins 0 .. mid_bin (8 kHz) */
    const int mid_bin = (int)(8000.0 * SPECTRUM_FFT_SIZE / 44100.0);
    int per = (mid_bin + band_count - 1) / band_count;   /* bins per band */
    if (per < 1) per = 1;

    for (int b = 0; b < band_count; b++) {
        int start = b * per;
        int end = start + per;
        if (end > mid_bin + 1) end = mid_bin + 1;
        double sum = 0.0;
        int cnt = 0;
        for (int i = start; i < end; i++) {
            sum += sqrtf(re[i] * re[i] + im[i] * im[i]);
            cnt++;
        }
        if (cnt == 0) { bands[b] = 0.0f; continue; }
        float v = (float)(sum / cnt) * norm;
        if (v > 1.0f) v = 1.0f;
        if (v < 0.0f) v = 0.0f;
        bands[b] = v;
    }
}
