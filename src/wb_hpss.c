/* wb_hpss.c — R020-A Harmonic-Percussive Source Separation (pure C11).
 * See wbus_hpss.h for the algorithm description. */
#include "wbus/wbus_hpss.h"
#include "wbus/wbus_fft.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

struct wb_hpss {
    int n;            /* FFT size */
    int hop;          /* hop = n/4 */
    int nbins;        /* n/2 + 1 */
    float sr;
    int h_len, p_len; /* median half-windows */
    double *win;      /* Hann window length n */
    wb_fft_plan *plan;
};

static double hann(int i, int n) { return 0.5 - 0.5 * cos(2.0 * M_PI * i / (n - 1)); }

/* in-place median of a small buffer (insertion sort, fine for ~63 elem) */
static double median_small(double *buf, int len) {
    for (int i = 1; i < len; i++) {
        double v = buf[i]; int j = i - 1;
        while (j >= 0 && buf[j] > v) { buf[j + 1] = buf[j]; j--; }
        buf[j + 1] = v;
    }
    return (len & 1) ? buf[len / 2] : 0.5 * (buf[len / 2 - 1] + buf[len / 2]);
}

wb_hpss *wb_hpss_create(int frame_size, float sample_rate, int h_len, int p_len) {
    if (frame_size < 64 || (frame_size & (frame_size - 1))) return NULL;
    wb_hpss *h = (wb_hpss*)calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->n = frame_size; h->hop = frame_size / 4; h->nbins = frame_size / 2 + 1;
    h->sr = sample_rate;
    h->h_len = h_len > 0 ? h_len : 31;
    h->p_len = p_len > 0 ? p_len : 31;
    h->win = (double*)malloc(sizeof(double) * h->n);
    for (int i = 0; i < h->n; i++) h->win[i] = hann(i, h->n);
    h->plan = wb_fft_create(h->n);
    if (!h->win || !h->plan) { wb_hpss_destroy(h); return NULL; }
    return h;
}

void wb_hpss_destroy(wb_hpss *h) {
    if (!h) return;
    free(h->win);
    if (h->plan) wb_fft_destroy(h->plan);
    free(h);
}

int wb_hpss_separate(wb_hpss *h, const float *in, uint32_t frames,
                     float *harmonic, float *percussive) {
    if (!h || !in || !harmonic || !percussive) return -1;
    int n = h->n, hop = h->hop, nb = h->nbins;
    int nframes = (int)((frames + hop - 1) / hop) + 1;
    if (nframes < 1) return -1;

    double *mag = (double*)calloc((size_t)nframes * nb, sizeof(double));
    double *ph  = (double*)calloc((size_t)nframes * nb, sizeof(double));
    double *hre = (double*)malloc(sizeof(double) * n);
    double *him = (double*)malloc(sizeof(double) * n);
    double *x   = (double*)malloc(sizeof(double) * n);
    if (!mag || !ph || !hre || !him || !x) { free(mag); free(ph); free(hre); free(him); free(x); return -1; }

    /* 1. forward STFT -> magnitude + phase */
    for (int t = 0; t < nframes; t++) {
        int start = t * hop;
        for (int i = 0; i < n; i++) {
            double s = (start + i < (int)frames) ? in[start + i] : 0.0;
            x[i] = s * h->win[i];
        }
        wb_fft_real(h->plan, x, hre, him);
        for (int b = 0; b < nb; b++) {
            double re = hre[b], im = him[b];
            mag[t * nb + b] = sqrt(re * re + im * im);
            ph[t * nb + b]  = atan2(im, re);
        }
    }

    /* 2. harmonic estimate: median along frequency axis (horizontal) */
    double *hmag = (double*)malloc(sizeof(double) * (size_t)nframes * nb);
    double *pmag = (double*)malloc(sizeof(double) * (size_t)nframes * nb);
    double *tmp  = (double*)malloc(sizeof(double) * (2 * h->h_len + 1 > 2 * h->p_len + 1 ? 2 * h->h_len + 1 : 2 * h->p_len + 1));
    if (!hmag || !pmag || !tmp) { free(mag); free(ph); free(hre); free(him); free(x); free(hmag); free(pmag); free(tmp); return -1; }

    for (int t = 0; t < nframes; t++) {
        for (int b = 0; b < nb; b++) {
            int lo = b - h->h_len, hi = b + h->h_len, k = 0;
            if (lo < 0) lo = 0; if (hi >= nb) hi = nb - 1;
            for (int j = lo; j <= hi; j++) tmp[k++] = mag[t * nb + j];
            hmag[t * nb + b] = median_small(tmp, k);
        }
    }
    /* percussive estimate: median of (mag - hmag) along time axis (vertical) */
    for (int b = 0; b < nb; b++) {
        for (int t = 0; t < nframes; t++) {
            int lo = t - h->p_len, hi = t + h->p_len, k = 0;
            if (lo < 0) lo = 0; if (hi >= nframes) hi = nframes - 1;
            for (int j = lo; j <= hi; j++) {
                double r = mag[j * nb + b] - hmag[j * nb + b];
                if (r < 0) r = 0;
                tmp[k++] = r;
            }
            pmag[t * nb + b] = median_small(tmp, k);
        }
    }

    /* reconstruct each component with original phase (windowed overlap-add) */
    double *wsum = (double*)calloc(n, sizeof(double)); /* accumulated window power */
    if (!wsum) { free(mag); free(ph); free(hre); free(him); free(x); free(hmag); free(pmag); free(tmp); free(wsum); return -1; }
    memset(harmonic, 0, sizeof(float) * frames);
    memset(percussive, 0, sizeof(float) * frames);

    for (int t = 0; t < nframes; t++) {
        int start = t * hop;
        for (int b = 0; b < nb; b++) {
            double m = mag[t * nb + b];
            double H = hmag[t * nb + b], P = pmag[t * nb + b];
            double hm = (H + P > 1e-9) ? m * H / (H + P) : m;
            double ang = ph[t * nb + b];
            hre[b] = hm * cos(ang); him[b] = hm * sin(ang); /* upper half stays
                                                              conjugate-symmetric */
        }
        wb_fft_real_inverse(h->plan, hre, him, x);
        for (int i = 0; i < n; i++) {
            if (start + i < (int)frames) {
                double v = x[i] * h->win[i];
                harmonic[start + i] += (float)v;
                wsum[(start + i) % n] += h->win[i] * h->win[i];
            }
        }
    }
    for (int t = 0; t < nframes; t++) {
        int start = t * hop;
        for (int b = 0; b < nb; b++) {
            double m = mag[t * nb + b];
            double H = hmag[t * nb + b], P = pmag[t * nb + b];
            double hm = (H + P > 1e-9) ? m * H / (H + P) : m;
            double pm = m - hm;
            double ang = ph[t * nb + b];
            hre[b] = pm * cos(ang); him[b] = pm * sin(ang);
        }
        wb_fft_real_inverse(h->plan, hre, him, x);
        for (int i = 0; i < n; i++) {
            if (start + i < (int)frames) {
                double v = x[i] * h->win[i];
                percussive[start + i] += (float)v;
                wsum[(start + i) % n] += h->win[i] * h->win[i];
            }
        }
    }

    /* The overlap-add is correctly scaled by the IFFT normalization (matching
     * the proven voice_isolate pattern); no extra window-power division. */
    free(mag); free(ph); free(hre); free(him); free(x);
    free(hmag); free(pmag); free(tmp); free(wsum);
    return 0;
}
