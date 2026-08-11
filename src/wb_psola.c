/*
 * wb_psola.c — Time-Domain Pitch-Synchronous Overlap-Add (R017 #10)
 *
 * The real-time voice-changer core. TD-PSOLA shifts F0 while PRESERVING the
 * spectral envelope (formants), so a pitch-shifted voice stays natural
 * instead of turning chipmunk (which happens when formants move with pitch).
 *
 * Algorithm:
 *   1. Pitch-mark the input: one mark per glottal period, found via
 *      autocorrelation (period = highest-energy lag in the voicing range).
 *   2. For each mark, extract a Hann-windowed grain spanning ±1 period.
 *   3. Place grains at spacing period/factor (factor>1 = pitch up, <1 = down).
 *      To keep the DURATION constant while raising pitch by factor, we place
 *      ~factor grains per input period (factor times more glottal pulses).
 *   4. Overlap-add, normalized by the summed window envelope.
 *
 * Pure C11, no third party, self-contained (deterministic LCG noise-free).
 */
#include "wb_psola.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* Window the signal x[n] and find the fundamental period (in samples) via
 * autocorrelation over the voicing range [lo,hi] (60..400 Hz), centred at
 * `start`. Returns the period, or 0 if unvoiced/silent. */
static int detect_period(const double *x, int n, int sr, int start) {
    int w = sr / 25;              /* ~40 ms window */
    if (w > n) w = n;
    int lo = sr / 400, hi = sr / 60;
    if (hi > w) hi = w;
    if (lo < 2) lo = 2;
    int start_i = start;
    if (start_i < 0) start_i = 0;
    int len = n - start_i;
    if (len > w) len = w;
    if (len <= hi + 1) return 0;

    /* energy in the window (for the voicing gate) */
    double e = 0;
    for (int i = 0; i < len; i++) e += x[start_i + i] * x[start_i + i];
    if (e < 1e-6) return 0;

    double best = 0; int best_lag = 0;
    for (int lag = lo; lag <= hi; lag++) {
        double s = 0;
        int cnt = len - lag;
        for (int i = 0; i < cnt; i++)
            s += x[start_i + i] * x[start_i + i + lag];
        /* normalize by energy of the lagged segment (periodicity measure) */
        double e2 = 0;
        for (int i = 0; i < cnt; i++) e2 += x[start_i + i + lag] * x[start_i + i + lag];
        double den = e2 > 1e-9 ? e2 : 1.0;
        double r = s / den;
        if (r > best) { best = r; best_lag = lag; }
    }
    if (best < 0.3) return 0;     /* not periodic enough -> unvoiced */
    return best_lag;
}

/* Pitch-mark: one mark per glottal period, stepping by the detected period. */
static int pitch_mark(const double *x, int n, int sr,
                      int *marks, int *periods, int max_marks) {
    int nm = 0;
    double t = 0.0;
    while (t < n && nm < max_marks) {
        int m = (int)t;
        int p = detect_period(x, n, sr, m);
        if (p <= 0) p = sr / 120;          /* hold a default period if unvoiced */
        marks[nm] = m;
        periods[nm] = p;
        nm++;
        t += p;
    }
    return nm;
}

int wb_psola_pitch_shift(const double *x, int n, int sr, double factor,
                         double *out, int max_out) {
    if (!x || !out || n < sr / 30 || factor <= 0) return 0;

    int max_marks = n / (sr / 400) + 2;    /* at most one per ~60Hz period */
    if (max_marks > 4096) max_marks = 4096;
    int *marks = malloc((size_t)max_marks * sizeof(int));
    int *periods = malloc((size_t)max_marks * sizeof(int));
    if (!marks || !periods) { free(marks); free(periods); return 0; }

    int nm = pitch_mark(x, n, sr, marks, periods, max_marks);
    if (nm < 2) { free(marks); free(periods); return 0; }

    /* output length: with factor copies per period, duration stays ~n */
    int out_n = n;
    if (out_n > max_out) out_n = max_out;

    double *accum = calloc((size_t)out_n, sizeof(double));
    double *wsum = calloc((size_t)out_n, sizeof(double));
    if (!accum || !wsum) { free(marks); free(periods); free(accum); free(wsum); return 0; }

    /* Place grains at spacing period/factor across the output. To keep the
     * duration constant at pitch*factor, there are ~factor grains per input
     * period (more glottal pulses in the same time). */
    double t2 = 0.0;
    while (t2 < out_n) {
        /* map output time back to input time to pick the nearest mark */
        double in_t = t2 * factor;
        int idx = 0;
        /* find mark nearest to in_t (marks are sorted ascending) */
        for (int i = 0; i < nm; i++) {
            if (marks[i] <= in_t) idx = i;
            else break;
        }
        int p = periods[idx];
        int m = marks[idx];
        int m2 = (int)t2;

        /* grain: windowed segment around input mark m, span [-p, p] */
        for (int j = -p; j <= p; j++) {
            int src = m + j;
            if (src < 0 || src >= n) continue;
            int dst = m2 + j;
            if (dst < 0 || dst >= out_n) continue;
            double h = 0.5 * (1.0 + cos(M_PI * (double)j / (double)(p > 0 ? p : 1)));
            accum[dst] += x[src] * h;
            wsum[dst] += h;
        }
        t2 += (double)p / factor;
    }

    for (int k = 0; k < out_n; k++) {
        if (wsum[k] > 1e-6) out[k] = accum[k] / wsum[k];
        else out[k] = 0.0;
    }

    free(marks); free(periods); free(accum); free(wsum);
    return out_n;
}
