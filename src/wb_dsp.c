/*
 * wb_dsp.c — DSP primitives: autocorrelation, Levinson-Durbin LPC,
 * radix-2 FFT, YIN pitch, peak picking. Strict C11, no third party.
 */
#include "wb_dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void wb_acorr(const double *x, size_t n, double *R, size_t maxlag) {
    for (size_t k = 0; k <= maxlag && k < n; k++) {
        double acc = 0.0;
        for (size_t i = 0; i + k < n; i++) acc += x[i] * x[i + k];
        R[k] = acc;
    }
}

int wb_lpc(const double *R, int p, double *a, double *E) {
    /* Levinson-Durbin recursion */
    double *err = malloc(((size_t)p + 1) * sizeof(double));
    double *rc  = malloc(((size_t)p + 1) * sizeof(double)); /* reflection coefs */
    double *tmp = malloc(((size_t)p + 1) * sizeof(double));
    if (!err || !rc || !tmp) { free(err); free(rc); free(tmp); return -1; }

    err[0] = R[0];
    if (err[0] <= 0) { free(err); free(rc); free(tmp); return -1; }

    for (int i = 1; i <= p; i++) {
        double acc = R[i];
        for (int j = 1; j < i; j++) acc -= a[j - 1] * R[i - j];
        double denom = err[i - 1];
        if (fabs(denom) < 1e-12) { free(err); free(rc); free(tmp); return -1; }
        rc[i] = acc / denom;
        /* update a */
        memcpy(tmp, a, ((size_t)i - 1) * sizeof(double));
        for (int j = 1; j < i; j++) a[j - 1] = tmp[j - 1] - rc[i] * tmp[i - j - 1];
        a[i - 1] = rc[i];
        err[i] = err[i - 1] * (1.0 - rc[i] * rc[i]);
        if (err[i] <= 0) { free(err); free(rc); free(tmp); return -1; }
    }
    *E = err[p];
    free(err); free(rc); free(tmp);
    return 0;
}

int wb_rfft(const double *x, size_t n, double *Re, double *Im) {
    if (n < 2 || (n & (n - 1)) != 0) return -1; /* not power of 2 */
    size_t N = n;

    /* copy into Re/Im working buffers (Re holds real, Im holds imag) */
    for (size_t i = 0; i < N; i++) { Re[i] = x[i]; Im[i] = 0.0; }

    /* bit-reversal permutation */
    for (size_t i = 1, j = 0; i < N; i++) {
        size_t bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double tr = Re[i]; Re[i] = Re[j]; Re[j] = tr;
            double ti = Im[i]; Im[i] = Im[j]; Im[j] = ti;
        }
    }

    /* iterative radix-2 */
    for (size_t len = 2; len <= N; len <<= 1) {
        double ang = -2.0 * M_PI / (double)len;
        double wr = cos(ang), wi = sin(ang);
        for (size_t i = 0; i < N; i += len) {
            double cur_wr = 1.0, cur_wi = 0.0;
            for (size_t k = 0; k < len / 2; k++) {
                size_t a = i + k, b = i + k + len / 2;
                double tr = Re[b] * cur_wr - Im[b] * cur_wi;
                double ti = Re[b] * cur_wi + Im[b] * cur_wr;
                Re[b] = Re[a] - tr; Im[b] = Im[a] - ti;
                Re[a] = Re[a] + tr; Im[a] = Im[a] + ti;
                double nwr = cur_wr * wr - cur_wi * wi;
                cur_wi = cur_wr * wi + cur_wi * wr;
                cur_wr = nwr;
            }
        }
    }
    return 0;
}

double wb_yin_f0(const double *x, size_t n, int sample_rate) {
    /* difference function d(tau) = sum (x[i]-x[i+tau])^2 */
    size_t tau_max = (size_t)(sample_rate / 50);   /* F0 >= 50 Hz */
    size_t tau_min = (size_t)(sample_rate / 500);  /* F0 <= 500 Hz */
    if (tau_max >= n) tau_max = n / 2;
    if (tau_min < 2) tau_min = 2;
    if (tau_max <= tau_min) return 0.0;

    double *d = malloc((tau_max + 1) * sizeof(double));
    double *cmnd = malloc((tau_max + 1) * sizeof(double));
    if (!d || !cmnd) { free(d); free(cmnd); return 0.0; }

    for (size_t tau = 0; tau <= tau_max; tau++) {
        double acc = 0.0;
        for (size_t i = 0; i + tau < n; i++) {
            double diff = x[i] - x[i + tau];
            acc += diff * diff;
        }
        d[tau] = acc;
    }

    /* cumulative mean normalized difference */
    cmnd[0] = 1.0;
    double running = 0.0;
    for (size_t tau = 1; tau <= tau_max; tau++) {
        running += d[tau];
        cmnd[tau] = running > 0 ? d[tau] * (double)tau / running : 1.0;
    }

    /* find the FIRST tau whose cmnd dips below threshold, then take the
     * LOCAL MINIMUM in its neighborhood (the classic YIN two-step) */
    double best_tau = 0.0;
    int found = 0;
    for (size_t tau = tau_min; tau < tau_max; tau++) {
        if (cmnd[tau] < 0.15) {
            /* walk forward to the local minimum */
            size_t best = tau;
            while (best + 1 < tau_max && cmnd[best + 1] < cmnd[best]) best++;
            /* parabolic interpolation around best for sub-sample precision */
            if (best > 0 && best + 1 < tau_max) {
                double a = cmnd[best - 1], b = cmnd[best], c = cmnd[best + 1];
                double den = a - 2 * b + c;
                if (fabs(den) > 1e-12) {
                    double off = 0.5 * (a - c) / den;   /* [-0.5, 0.5] */
                    best_tau = (double)best + off;
                } else {
                    best_tau = (double)best;
                }
            } else {
                best_tau = (double)best;
            }
            found = 1;
            break;
        }
    }
    free(d); free(cmnd);
    if (!found) return 0.0;
    if (best_tau < 1) return 0.0;
    return (double)sample_rate / best_tau;
}

size_t wb_argmax_abs(const double *x, size_t lo, size_t hi) {
    size_t best = lo;
    double bv = fabs(x[lo]);
    for (size_t i = lo + 1; i <= hi; i++) {
        double v = fabs(x[i]);
        if (v > bv) { bv = v; best = i; }
    }
    return best;
}
