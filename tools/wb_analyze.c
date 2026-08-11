/*
 * wb_analyze.c — Big Mac ABSORBER: demo.wav → voice-print
 *
 * Extracts the qualities of a vocal demo:
 *   - F0 contour (YIN) + stats
 *   - Formants F1-F3 (LPC, Levinson-Durbin + root solving via companion
 *     matrix eigenvalue-free approach: quadratic factors)
 *   - Jitter, shimmer, HNR (Praat-style formulas)
 * Prints a voice-print the engine can re-create from.
 *
 * Usage: wb_analyze <demo.wav>
 */
#include "wb_reader.h"
#include "wb_dsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* ---------- formant extraction via LPC frequency response ---------- */
/* Evaluate |H(e^{jw})| for the LPC synthesis filter 1/A(z) where
 * A(z) = 1 - sum_k a[k] z^-(k+1)  (Levinson-Durbin sign convention). */
static double lpc_gain_at(const double *a, int p, double f) {
    double w = 2.0 * M_PI * f;
    double re = 1.0, im = 0.0;
    double zr = cos(-w), zi = sin(-w);
    double pr = 1.0, pi = 0.0;   /* z^0 */
    for (int k = 0; k < p; k++) {
        /* z^-(k+1) = z^-k * z^-1 */
        double npr = pr * zr - pi * zi;
        double npi = pr * zi + pi * zr;
        pr = npr; pi = npi;
        re -= a[k] * pr;   /* MINUS: A(z) = 1 - sum a_k z^-k */
        im -= a[k] * pi;
    }
    double denom = re * re + im * im;
    return denom > 1e-12 ? 1.0 / sqrt(denom) : 0.0;
}

/* peak-pick the LPC spectrum for formants */
static void find_formants(const double *a, int p, int sample_rate,
                          double *F, int *nF, int maxF) {
    /* fine scan 50..4000 Hz in 5 Hz steps (speech formants live there) */
    double f_lo = 50.0, f_hi = 4000.0;
    if (f_hi > sample_rate / 2.0) f_hi = sample_rate / 2.0;
    int step = 5;
    int nsteps = (int)((f_hi - f_lo) / step);
    double *g = malloc((size_t)nsteps * sizeof(double));
    if (!g) { *nF = 0; return; }
    for (int i = 0; i < nsteps; i++) {
        double f = (f_lo + (double)i * step) / (double)sample_rate;
        g[i] = lpc_gain_at(a, p, f);
    }
    /* collect local maxima (strict), sorted by frequency */
    double *cands = malloc((size_t)nsteps * sizeof(double));
    if (!cands) { free(g); *nF = 0; return; }
    int count = 0;
    for (int i = 1; i < nsteps - 1; i++) {
        if (g[i] >= g[i-1] && g[i] > g[i+1]) {
            cands[count++] = f_lo + (double)i * step;
        }
    }
    /* formants are the LOWEST maxF peaks (F1 < F2 < F3) */
    int n = 0;
    for (int i = 0; i < count && n < maxF; i++) {
        F[n++] = cands[i];
    }
    *nF = n;
    free(g); free(cands);
}

/* ---------- jitter / shimmer / HNR (Praat-style) ---------- */
static void voice_quality(const double *x, size_t n, int sample_rate,
                          double *jitt, double *shim, double *hnr) {
    /* pitch marks via peak-picking on the envelope */
    size_t min_period = (size_t)(sample_rate / 500.0);
    size_t max_period = (size_t)(sample_rate / 50.0);
    if (max_period >= n) max_period = n / 2;

    /* detect glottal pulses: moving-average envelope + local peaks
     * (Teixeira method: remove linear trend, smooth ~10ms, find peaks) */
    size_t *marks = malloc(n * sizeof(size_t));
    double *amps = malloc(n * sizeof(double));
    if (!marks || !amps) { *jitt = 0; *shim = 0; *hnr = 0; free(marks); free(amps); return; }

    /* 1. detrend (remove linear trend) */
    double *det = malloc(n * sizeof(double));
    if (!det) { free(marks); free(amps); *jitt = 0; *shim = 0; *hnr = 0; return; }
    {
        double sumx = 0, sumy = 0, sumxx = 0, sumxy = 0;
        for (size_t k = 0; k < n; k++) { sumx += (double)k; sumy += x[k]; sumxx += (double)k * k; sumxy += (double)k * x[k]; }
        double den = (double)n * sumxx - sumx * sumx;
        double slope = den != 0 ? ((double)n * sumxy - sumx * sumy) / den : 0.0;
        double inter = (sumy - slope * sumx) / (double)n;
        for (size_t k = 0; k < n; k++) det[k] = x[k] - (slope * (double)k + inter);
    }

    /* 2. moving average ~10 ms */
    size_t win_ma = (size_t)(sample_rate * 0.010);
    if (win_ma < 1) win_ma = 1;
    double *env = malloc(n * sizeof(double));
    if (!env) { free(det); free(marks); free(amps); *jitt = 0; *shim = 0; *hnr = 0; return; }
    {
        double acc = 0;
        for (size_t k = 0; k < n; k++) {
            acc += fabs(det[k]);
            if (k >= win_ma) acc -= fabs(det[k - win_ma]);
            env[k] = acc / (double)(k < win_ma ? k + 1 : win_ma);
        }
    }

    /* 3. peaks: pitch-synchronous — search window ~1.5x the measured
     * period (from YIN), env max then refine to max |x| within ±15 */
    double f0_hint = wb_yin_f0(x, n, sample_rate);
    size_t period = f0_hint > 0 ? (size_t)(sample_rate / f0_hint) : (min_period + max_period) / 2;
    if (period < min_period) period = min_period;
    if (period > max_period) period = max_period;
    size_t step = (size_t)(period * 1.5);
    if (step < 1) step = 1;

    size_t nm = 0;
    size_t i = 0;
    while (i + period < n) {
        /* find env max in [i, i+period] */
        size_t emax = i;
        for (size_t k = i + 1; k <= i + period; k++) if (env[k] > env[emax]) emax = k;
        /* refine: max |x| in emax±15 */
        size_t lo = emax > 15 ? emax - 15 : 0;
        size_t hi = emax + 15 < n ? emax + 15 : n - 1;
        size_t best = wb_argmax_abs(x, lo, hi);
        /* require it to be a real pulse (above noise floor) */
        if (fabs(x[best]) > 0.005 * (fabs(x[0]) + 1e-9 + 0.0)) {
            marks[nm] = best;
            amps[nm] = fabs(x[best]);
            nm++;
        }
        i = emax + step;  /* move past this pulse */
    }
    free(det); free(env);
    if (nm < 3) { *jitt = 0; *shim = 0; *hnr = 0; free(marks); free(amps); return; }

    /* periods in samples */
    double *periods = malloc((nm > 0 ? nm - 1 : 0) * sizeof(double));
    if (!periods) { free(marks); free(amps); *jitt = 0; *shim = 0; *hnr = 0; return; }
    for (size_t k = 0; k + 1 < nm; k++) periods[k] = (double)(marks[k+1] - marks[k]);

    /* jitt (local, %) = mean |T_i - T_{i-1}| / mean T * 100 */
    double meanT = 0; for (size_t k = 0; k + 1 < nm; k++) meanT += periods[k];
    meanT /= (double)(nm - 1);
    double jacc = 0;
    for (size_t k = 1; k + 1 < nm; k++) jacc += fabs(periods[k] - periods[k-1]);
    *jitt = meanT > 0 ? jacc / (double)(nm - 2) / meanT * 100.0 : 0.0;

    /* shim (local, %) = mean |A_i - A_{i-1}| / mean A * 100 */
    double meanA = 0; for (size_t k = 0; k < nm; k++) meanA += amps[k];
    meanA /= (double)nm;
    double sacc = 0;
    for (size_t k = 1; k < nm; k++) sacc += fabs(amps[k] - amps[k-1]);
    *shim = meanA > 0 ? sacc / (double)(nm - 1) / meanA * 100.0 : 0.0;

    /* HNR via autocorrelation (Boersma): HNR = 10 log10( AC(T0) / (AC(0)-AC(T0)) ) */
    size_t maxlag = (size_t)(sample_rate / 50.0);
    double *R = malloc((maxlag + 1) * sizeof(double));
    if (R) {
        wb_acorr(x, n, R, maxlag);
        /* find period = argmax of AC in [min_period, max_period] */
        size_t bestT = min_period;
        for (size_t t = min_period + 1; t <= max_period && t <= n; t++) {
            if (R[t] > R[bestT]) bestT = t;
        }
        double ac0 = R[0], acT = R[bestT];
        if (ac0 > acT && ac0 > 0 && acT >= 0) {
            *hnr = 10.0 * log10(acT / (ac0 - acT));
        } else {
            *hnr = 0.0;
        }
        free(R);
    } else {
        *hnr = 0.0;
    }

    free(periods); free(marks); free(amps);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: wb_analyze <demo.wav>\n");
        return 1;
    }
    wb_audio_t a;
    if (wb_audio_read(argv[1], &a) != 0) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    printf("demo: %s  (%d Hz, %d ch, %.2f s)\n", argv[1],
           a.sample_rate, a.channels, (double)a.n / a.sample_rate);

    /* F0 via YIN on the whole signal (windowed) */
    size_t win = a.sample_rate / 2;  /* 500 ms window */
    if (win > a.n) win = a.n;
    double f0 = wb_yin_f0(a.data, win, a.sample_rate);
    printf("F0 (YIN): %.1f Hz\n", f0);

    /* LPC formants on a short voiced window (~30 ms = 2-3 pitch periods;
     * a long window makes autocorrelation model pitch, not formants).
     * Order: sr/1000+4 rule of thumb (44.1k -> ~48; 24-28 is enough here). */
    int p = 24;
    size_t fwin = (size_t)(a.sample_rate * 0.030);
    if (fwin > a.n) fwin = a.n;
    /* center the window in the middle half of the file (steady region) */
    size_t wstart = a.n / 4;
    if (wstart + fwin > a.n) wstart = a.n - fwin;
    double *R = malloc(((size_t)p + 1) * sizeof(double));
    double *coef = malloc((size_t)p * sizeof(double));
    if (R && coef) {
        /* pre-emphasis + window */
        double *w = malloc(fwin * sizeof(double));
        if (w) {
            for (size_t i = 0; i < fwin; i++) {
                double pre = a.data[wstart + i] - (i > 0 ? 0.97 * a.data[wstart + i - 1] : 0);
                double hann = 0.5 * (1 - cos(2 * M_PI * i / (fwin - 1)));
                w[i] = pre * hann;
            }
            wb_acorr(w, fwin, R, (size_t)p);
            double E = 0;
            memset(coef, 0, (size_t)p * sizeof(double));
            if (wb_lpc(R, p, coef, &E) == 0) {
                double F[5]; int nF = 0;
                find_formants(coef, p, a.sample_rate, F, &nF, 4);
                printf("formants (LPC-%d, 30ms):", p);
                for (int k = 0; k < nF; k++) printf(" F%d=%.0f", k + 1, F[k]);
                printf("\n");
            } else {
                printf("formants (LPC-%d): FAILED (singular)\n", p);
            }
            free(w);
        }
        free(R); free(coef);
    }

    /* voice quality on a sustained middle section */
    double jitt = 0, shim = 0, hnr = 0;
    size_t start = a.n / 4;
    size_t len = a.n / 2;
    if (len > a.n - start) len = a.n - start;
    if (len > 0) voice_quality(a.data + start, len, a.sample_rate, &jitt, &shim, &hnr);
    printf("quality: jitter=%.2f%%  shimmer=%.2f%%  HNR=%.1f dB\n", jitt, shim, hnr);

    wb_audio_free(&a);
    return 0;
}
