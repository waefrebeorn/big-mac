/* wb_analysis.c — broadcast/metering-style audio analysis tools.
 * Pure C11, zero third-party. Reuses wb_fft (wbus_fft.h) for spectrum.
 *
 * Functions:
 *   wb_analysis_loudness   — simplified BS.1770-4 K-weighted integrated LUFS
 *   wb_analysis_peak       — max abs sample in dBFS
 *   wb_analysis_rms        — sqrt(mean square) in dBFS
 *   wb_analysis_crest_factor — peak minus RMS in dB
 *   wb_analysis_spectrum   — FFT magnitude in num_bins octave bands
 *   wb_analysis_phase_correlation — -1..+1 mono compatibility check
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "wbus/wbus.h"
#include "wbus/wbus_fft.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DBFLOOR -120.0f

static inline float lin2db(float x) {
    if (x <= 0.0f) return DBFLOOR;
    return 20.0f * log10f(x);
}

/* ---- K-weighting biquad section ---- */
/* BS.1770-4 K-weighting: pre-filter (high-shelf +4dB @ 1513 Hz) + RLB
 * high-pass (60 Hz, Q=0.707). We implement both as cascaded IIR biquads
 * applied sample-by-sample. */
typedef struct {
    /* biquad coefficients (Direct Form II transposed) */
    float b0, b1, b2, a1, a2;
    float z1, z2; /* state */
} kweight_biquad;

static void kweight_init(kweight_biquad *f, float sr) {
    /* Pre-filter: high-shelf, f0=1513 Hz, gain +4 dB, Q=0.707 (approx shelf) */
    double w0 = 2.0 * M_PI * 1513.0 / sr;
    double A  = pow(10.0, 4.0 / 40.0); /* +4 dB */
    double sinw = sin(w0), cosw = cos(w0);
    double alpha = sinw / (2.0 * 0.707);
    double sqrtA = sqrt(A);
    double b0 = A * ((A + 1.0) - (A - 1.0) * cosw + 2.0 * sqrtA * alpha);
    double b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw);
    double b2 = A * ((A + 1.0) + (A - 1.0) * cosw - 2.0 * sqrtA * alpha);
    double a0 = (A + 1.0) - (A - 1.0) * cosw + 2.0 * A * alpha;
    double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw);
    double a2 = (A + 1.0) + (A - 1.0) * cosw - 2.0 * A * alpha;
    f->b0 = (float)(b0/a0); f->b1 = (float)(b1/a0); f->b2 = (float)(b2/a0);
    f->a1 = (float)(a1/a0); f->a2 = (float)(a2/a0);
    f->z1 = f->z2 = 0.0f;
}

static void kweight_rlb_init(kweight_biquad *f, float sr) {
    /* RLB filter: high-pass, f0=60 Hz, Q=0.707 */
    double w0 = 2.0 * M_PI * 60.0 / sr;
    double cosw = cos(w0);
    double alpha = sin(w0) / (2.0 * 0.707);
    double b0 = (1.0 + cosw) / 2.0;
    double b1 = -(1.0 + cosw);
    double b2 = (1.0 + cosw) / 2.0;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cosw;
    double a2 = 1.0 - alpha;
    f->b0 = (float)(b0/a0); f->b1 = (float)(b1/a0); f->b2 = (float)(b2/a0);
    f->a1 = (float)(a1/a0); f->a2 = (float)(a2/a0);
    f->z1 = f->z2 = 0.0f;
}

static inline float kweight_process(kweight_biquad *f, float in) {
    float out = f->b0 * in + f->z1;
    f->z1 = f->b1 * in - f->a1 * out + f->z2;
    f->z2 = f->b2 * in - f->a2 * out;
    return out;
}

/* ---- Loudness (simplified BS.1770-4) ---- */
int wb_analysis_loudness(const float *audio, int n, float sr, float *lufs_out) {
    if (!audio || n <= 0 || sr <= 0.0f || !lufs_out) return -1;

    kweight_biquad pre, rlb;
    kweight_init(&pre, sr);
    kweight_rlb_init(&rlb, sr);

    /* Gate block = 400 ms (BS.1770 short-term). We accumulate mean-square
     * over the entire buffer as a single integrated estimate (simplified:
     * no absolute/relative gating — suitable for metering, not full
     * compliance measurement). */
    double sum_sq = 0.0;
    for (int i = 0; i < n; i++) {
        float x = audio[i];
        x = kweight_process(&pre, x);
        x = kweight_process(&rlb, x);
        sum_sq += (double)x * (double)x;
    }
    double ms = sum_sq / n;
    if (ms <= 0.0) { *lufs_out = DBFLOOR; return 0; }
    *lufs_out = (float)(-0.691 + 10.0 * log10(ms));
    return 0;
}

/* ---- Peak (dBFS) ---- */
int wb_analysis_peak(const float *audio, int n, float *peak_db) {
    if (!audio || n <= 0 || !peak_db) return -1;
    float peak = 0.0f;
    for (int i = 0; i < n; i++) {
        float a = audio[i] >= 0.0f ? audio[i] : -audio[i];
        if (a > peak) peak = a;
    }
    *peak_db = lin2db(peak);
    return 0;
}

/* ---- RMS (dBFS) ---- */
int wb_analysis_rms(const float *audio, int n, float *rms_db) {
    if (!audio || n <= 0 || !rms_db) return -1;
    double sum_sq = 0.0;
    for (int i = 0; i < n; i++) {
        double x = (double)audio[i];
        sum_sq += x * x;
    }
    float rms = (float)sqrt(sum_sq / n);
    *rms_db = lin2db(rms);
    return 0;
}

/* ---- Crest factor (peak - RMS in dB) ---- */
int wb_analysis_crest_factor(const float *audio, int n, float *crest_db) {
    if (!audio || n <= 0 || !crest_db) return -1;
    float peak_db, rms_db;
    if (wb_analysis_peak(audio, n, &peak_db) != 0) return -1;
    if (wb_analysis_rms(audio, n, &rms_db) != 0) return -1;
    *crest_db = peak_db - rms_db;
    return 0;
}

/* ---- Spectrum (FFT magnitude in octave bands) ---- */
int wb_analysis_spectrum(const float *audio, int n, float *bins, int num_bins,
                         float sr) {
    if (!audio || n <= 0 || !bins || num_bins <= 0 || sr <= 0.0f) return -1;

    /* Find next power of two >= n for FFT */
    int fft_size = 1;
    while (fft_size < n) fft_size <<= 1;

    /* Allocate FFT buffers */
    double *re = (double*)calloc(fft_size, sizeof(double));
    double *im = (double*)calloc(fft_size, sizeof(double));
    if (!re || !im) { free(re); free(im); return -1; }

    /* Copy audio (zero-padded) */
    for (int i = 0; i < n; i++) re[i] = (double)audio[i];

    /* Create FFT plan and run */
    wb_fft_plan *plan = wb_fft_create(fft_size);
    if (!plan) { free(re); free(im); return -1; }
    wb_fft_run(plan, re, im, 0);
    wb_fft_destroy(plan);

    /* Bin the magnitude spectrum into octave-spaced bands.
     * Band b covers [f_lo * ratio^b, f_lo * ratio^(b+1)) where
     * ratio = 2^(1/num_bins) and f_lo = 20 Hz (lowest audible).
     * Bands above nyquist are left at DBFLOOR. */
    double nyquist = sr / 2.0;
    double f_lo = 20.0; /* lowest band edge */

    memset(bins, 0, (size_t)num_bins * sizeof(float));
    int *bin_counts = (int*)calloc((size_t)num_bins, sizeof(int));
    if (!bin_counts) { free(re); free(im); return -1; }

    int usable_bins = fft_size / 2;
    for (int k = 1; k < usable_bins; k++) {
        double freq = (double)k * sr / (double)fft_size;
        if (freq < f_lo || freq >= nyquist) continue;
        /* Find which octave band this frequency falls into */
        int b = (int)(log2(freq / f_lo) * num_bins);
        if (b < 0) b = 0;
        if (b >= num_bins) b = num_bins - 1;
        double mag = sqrt(re[k]*re[k] + im[k]*im[k]);
        bins[b] += (float)mag;
        bin_counts[b]++;
    }

    /* Average magnitude per bin and convert to dB */
    for (int b = 0; b < num_bins; b++) {
        if (bin_counts[b] > 0) {
            bins[b] /= (float)bin_counts[b];
            bins[b] = lin2db(bins[b]);
        } else {
            bins[b] = DBFLOOR;
        }
    }

    free(bin_counts);
    free(re);
    free(im);
    return 0;
}

/* ---- Phase correlation (-1 to +1) ---- */
int wb_analysis_phase_correlation(const float *l, const float *r, int n,
                                  float *correlation) {
    if (!l || !r || n <= 0 || !correlation) return -1;
    double sum_ll = 0.0, sum_rr = 0.0, sum_lr = 0.0;
    for (int i = 0; i < n; i++) {
        double dl = (double)l[i];
        double dr = (double)r[i];
        sum_ll += dl * dl;
        sum_rr += dr * dr;
        sum_lr += dl * dr;
    }
    double denom = sqrt(sum_ll * sum_rr);
    if (denom <= 0.0) { *correlation = 0.0f; return 0; }
    *correlation = (float)(sum_lr / denom);
    return 0;
}