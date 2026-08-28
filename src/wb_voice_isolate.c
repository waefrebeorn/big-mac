/* wb_voice_isolate.c — R018-D spectral voice isolation (RNNoise-style).
 *
 * Pipeline per channel:
 *   1. Frame the signal (Hann window, 50% overlap, N=1024 @ 44.1k ≈ 23ms).
 *   2. Real FFT -> magnitude + phase per bin.
 *   3. Track a slow-moving noise-floor magnitude per bin (min-tracking
 *      with decay) — this is the spectral noise estimate.
 *   4. Wiener-style soft gain: g = mag^2 / (mag^2 + k*floor^2), clamped by
 *      the global `reduction` and a hard `floor_db` gate. This attenuates
 *      bins that sit at/below the estimated noise floor (noise/breath/hiss)
 *      while preserving speech formants.
 *   5. Reconstruct via IFFT (overlap-add).
 *
 * No third-party deps; uses our own wb_fft. */
#include "wbus/wbus_voice_isolate.h"
#include "wbus/wbus_fft.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define ISO_N 1024          /* FFT / frame size */
#define ISO_HOP (ISO_N / 2) /* 50% overlap */
#define ISO_NBINS (ISO_N / 2 + 1)

struct wb_isolate {
    float reduction;        /* 0..1 suppression strength */
    float floor_db;         /* gate threshold dB */
    float floor_lin;        /* linear gain floor (from floor_db) */
    float noise[ISO_NBINS]; /* per-bin tracked noise-floor magnitude */
    int   has_noise;        /* 0 until first frame seeds the floor */

    /* overlap-add ring state per channel */
    double win[ISO_N];      /* Hann window */
    double prev_re[ISO_N], prev_im[ISO_N];
    float  olap[ISO_N];     /* overlap buffer from previous frame */
    int    pos;             /* sample position in the hop cycle */
    /* input accumulator */
    float  acc[ISO_N];
    int    acc_n;
};

static double hann(int i, int n) {
    return 0.5 - 0.5 * cos(2.0 * M_PI * i / (n - 1));
}

wb_isolate *wb_isolate_create(float reduction, float floor_db) {
    wb_isolate *iso = (wb_isolate*)calloc(1, sizeof(*iso));
    if (!iso) return NULL;
    iso->reduction = (reduction < 0.0f) ? 0.0f : (reduction > 1.0f ? 1.0f : reduction);
    iso->floor_db  = floor_db;
    iso->floor_lin = (float)pow(10.0, floor_db / 20.0);
    for (int i = 0; i < ISO_N; i++) iso->win[i] = hann(i, ISO_N);
    iso->pos = 0;
    iso->acc_n = 0;
    return iso;
}

void wb_isolate_destroy(wb_isolate *iso) { free(iso); }

/* Process one channel's worth of samples; `in`/`out` length `frames`. */
static void iso_channel(wb_isolate *iso, const float *in, float *out, int frames) {
    wb_fft_plan *plan = wb_fft_create(ISO_N);
    if (!plan) { for (int i = 0; i < frames; i++) out[i] = in[i]; return; }

    double *re = (double*)malloc(ISO_N * sizeof(double));
    double *im = (double*)malloc(ISO_N * sizeof(double));
    double *x  = (double*)malloc(ISO_N * sizeof(double));
    double *y  = (double*)malloc(ISO_N * sizeof(double));
    if (!re || !im || !x || !y) { for (int i = 0; i < frames; i++) out[i] = in[i];
                                 free(re); free(im); free(x); free(y); wb_fft_destroy(plan); return; }

    int idx = 0;
    for (int s = 0; s < frames; s++) {
        iso->acc[iso->acc_n++] = in[s];
        if (iso->acc_n < ISO_N) continue;

        /* window */
        for (int i = 0; i < ISO_N; i++) x[i] = iso->acc[i] * iso->win[i];
        iso->acc_n = 0;

        wb_fft_real(plan, x, re, im);

        /* per-bin spectral gate */
        float maxmag = 1e-9f;
        for (int b = 0; b < ISO_NBINS; b++) {
            double mag = sqrt(re[b]*re[b] + im[b]*im[b]);
            if (mag > maxmag) maxmag = (float)mag;
            if (!iso->has_noise) iso->noise[b] = (float)mag;
            else {
                /* min-tracking noise floor with slow rise (adapts to new noise) */
                float decay = 0.999f;
                float nf = iso->noise[b];
                if (mag < nf) nf = (float)(nf * decay + mag * (1.0 - decay));
                else          nf = (float)(nf * 0.9995 + mag * 0.0005); /* slow rise */
                iso->noise[b] = nf;
            }
        }
        iso->has_noise = 1;

        float red = iso->reduction;
        for (int b = 0; b < ISO_NBINS; b++) {
            double mag = sqrt(re[b]*re[b] + im[b]*im[b]);
            double nf  = iso->noise[b];
            /* Wiener estimate of speech presence */
            double sp = mag*mag;
            double nn = nf*nf * 3.0;                 /* noise variance scale */
            double g  = sp / (sp + nn);              /* 0..1 Wiener gain */
            /* global gate: kill bins below floor_db, scaled by reduction */
            double floor_g = (mag < iso->floor_lin * maxmag * 0.5) ? (1.0 - red) : 1.0;
            g *= floor_g;
            if (g > 1.0) g = 1.0;
            re[b] *= g; im[b] *= g;
        }

        wb_fft_real_inverse(plan, re, im, y);

        /* overlap-add with previous */
        for (int i = 0; i < ISO_N; i++) {
            double v = y[i] + iso->olap[i];
            if (idx < frames) out[idx++] = (float)(v * 0.5); /* overlap-add norm */
            iso->olap[i] = (float)(y[i] * 0.5);   /* carry remainder (next frame) */
        }
    }
    /* flush remaining accumulated samples (trailing partial frame) */
    for (int j = idx; j < frames; j++) out[j] = in[j]; /* partial last frame: pass through */

    free(re); free(im); free(x); free(y);
    wb_fft_destroy(plan);
}

void wb_isolate_process(wb_isolate *iso, const float *in, float *out, int frames) {
    iso_channel(iso, in, out, frames);
}

void wb_isolate_process_stereo(wb_isolate *iso, const float *in, float *out, int frames) {
    /* Per-channel spectral gate sharing one noise-floor estimate. */
    float *l  = (float*)malloc(frames * sizeof(float));
    float *r  = (float*)malloc(frames * sizeof(float));
    float *lo = (float*)malloc(frames * sizeof(float));
    float *ro = (float*)malloc(frames * sizeof(float));
    if (!l || !r || !lo || !ro) { for (int i = 0; i < 2*frames; i++) out[i] = in[i];
                                  free(l); free(r); free(lo); free(ro); return; }
    for (int i = 0; i < frames; i++) { l[i] = in[2*i]; r[i] = in[2*i+1]; }
    iso_channel(iso, l, lo, frames);
    iso_channel(iso, r, ro, frames);
    for (int i = 0; i < frames; i++) { out[2*i] = lo[i]; out[2*i+1] = ro[i]; }
    free(l); free(r); free(lo); free(ro);
}
