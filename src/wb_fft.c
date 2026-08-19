/* wb_fft.c — minimal self-contained radix-2 FFT (Cooley-Tukey).
 * Pure C11, no third-party. Powers of two only. */
#include "wbus/wbus_fft.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

static int is_pow2(int n) { return n > 1 && (n & (n - 1)) == 0; }

wb_fft_plan *wb_fft_create(int n) {
    if (!is_pow2(n)) return NULL;
    wb_fft_plan *p = (wb_fft_plan*)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->n = n;
    p->rev = (int*)malloc((size_t)n * sizeof(int));
    if (!p->rev) { wb_fft_destroy(p); return NULL; }

    /* bit-reversal permutation */
    int bits = 0;
    for (int v = n; v > 1; v >>= 1) bits++;
    for (int i = 0; i < n; i++) {
        int r = 0, x = i;
        for (int b = 0; b < bits; b++) { r = (r << 1) | (x & 1); x >>= 1; }
        p->rev[i] = r;
    }
    return p;
}

void wb_fft_destroy(wb_fft_plan *p) {
    if (!p) return;
    free(p->rev);
    free(p);
}

void wb_fft_run(const wb_fft_plan *p, double *re, double *im, int invert) {
    int n = p->n;
    /* bit-reversed copy */
    for (int i = 0; i < n; i++) {
        int j = p->rev[i];
        if (j > i) {
            double tr = re[i]; re[i] = re[j]; re[j] = tr;
            double ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1;
        double sign = invert ? 1.0 : -1.0;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; k++) {
                double ang = sign * 2.0 * M_PI * k / len;
                double c = cos(ang), s = sin(ang);
                double tre = re[i + k + half], tim = im[i + k + half];
                double wr = c * tre - s * tim;
                double wi = c * tim + s * tre;
                re[i + k + half] = re[i + k] - wr;
                im[i + k + half] = im[i + k] - wi;
                re[i + k]       += wr;
                im[i + k]       += wi;
            }
        }
    }
    if (invert) {
        double inv = 1.0 / n;
        for (int i = 0; i < n; i++) { re[i] *= inv; im[i] *= inv; }
    }
}

void wb_fft_real(const wb_fft_plan *p, const double *x, double *re, double *im) {
    int n = p->n;
    for (int i = 0; i < n; i++) { re[i] = x[i]; im[i] = 0.0; }
    wb_fft_run(p, re, im, 0);
}

void wb_fft_real_inverse(const wb_fft_plan *p, const double *re, const double *im,
                         double *out) {
    /* re/im are the (modified) spectrum; out receives the time signal.
     * Caller must ensure out does not alias re/im (use separate buffers). */
    int n = p->n;
    double *r = (double*)malloc((size_t)n * sizeof(double));
    double *m = (double*)malloc((size_t)n * sizeof(double));
    if (!r || !m) { for (int i = 0; i < n; i++) out[i] = 0.0; free(r); free(m); return; }
    for (int i = 0; i < n; i++) { r[i] = re[i]; m[i] = im[i]; }
    wb_fft_run(p, r, m, 1);   /* IFFT (already divided by n) */
    for (int i = 0; i < n; i++) out[i] = r[i];
    free(r); free(m);
}
