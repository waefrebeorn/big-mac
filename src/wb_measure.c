/*
 * wb_measure.c — voice-print measurement library (strict C11, no third party)
 *
 * Implements the analysis layer targets:
 *   A01 YIN F0, A03 autocorrelation, A04 pitch-marks, A05 voiced/unvoiced,
 *   A06 F0 smoothing/stats, A07 vibrato, A09/A10/A12/A13 formants + BW,
 *   A16 spectral tilt, A17/A18 jitter/shimmer, A19 HNR, A20 CPP, A21 H1-H2.
 */
#include "wb_measure.h"
#include "wb_dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- F0: YIN + stats + vibrato (A01-A08) ---------------- */

static double frame_f0(const double *x, size_t n, int sr) {
    return wb_yin_f0(x, n, sr);
}

wb_f0_measure_t wb_measure_f0(const double *x, size_t n, int sr) {
    wb_f0_measure_t m;
    memset(&m, 0, sizeof(m));

    size_t frame = (size_t)(sr * 0.040);      /* 40 ms frames */
    if (frame > n) frame = n;
    size_t hop = frame / 2;
    size_t nframes = n > frame ? 1 + (n - frame) / hop : 1;

    double *f0s = malloc(nframes * sizeof(double));
    int *voiced = malloc(nframes * sizeof(int));
    if (!f0s || !voiced) { free(f0s); free(voiced); return m; }

    int nv = 0;
    for (size_t f = 0; f < nframes; f++) {
        size_t start = f * hop;
        if (start + frame > n) start = n - frame;
        double f0 = frame_f0(x + start, frame, sr);
        /* voiced gate: periodicity confidence via normalized acorr peak */
        size_t maxlag = (size_t)(sr / 50.0);
        double *R = malloc((maxlag + 1) * sizeof(double));
        double conf = 0;
        if (R) {
            wb_acorr(x + start, frame, R, maxlag);
            size_t tmin = (size_t)(sr / 500.0), tmax = maxlag;
            if (tmax > frame) tmax = frame;
            size_t best = tmin;
            for (size_t t = tmin + 1; t <= tmax; t++) if (R[t] > R[best]) best = t;
            if (R[0] > 0) conf = R[best] / R[0];
            free(R);
        }
        f0s[f] = (f0 > 40 && f0 < 500 && conf > 0.35) ? f0 : 0.0;
        if (f0s[f] > 0) { voiced[f] = 1; nv++; } else voiced[f] = 0;
    }
    m.voiced_fraction = nframes > 0 ? (double)nv / nframes : 0.0;

    /* stats over voiced frames */
    int cnt = 0;
    double sum = 0, mn = 1e9, mx = 0;
    for (size_t f = 0; f < nframes; f++) {
        if (voiced[f]) {
            sum += f0s[f];
            if (f0s[f] < mn) mn = f0s[f];
            if (f0s[f] > mx) mx = f0s[f];
            cnt++;
        }
    }
    if (cnt > 0) {
        m.f0_mean = sum / cnt;
        m.f0_min = mn;
        m.f0_max = mx;
        double v = 0;
        for (size_t f = 0; f < nframes; f++)
            if (voiced[f]) { double d = f0s[f] - m.f0_mean; v += d * d; }
        m.f0_sd = sqrt(v / cnt);
        m.f0 = m.f0_mean;
    }

    /* vibrato (A07): F0 modulation in the 3-9 Hz band */
    if (cnt >= 4) {
        /* collect voiced frame times + f0 */
        double *ts = malloc((size_t)cnt * sizeof(double));
        double *fv = malloc((size_t)cnt * sizeof(double));
        int k = 0;
        for (size_t f = 0; f < nframes; f++) if (voiced[f]) {
            ts[k] = (double)(f * hop) / sr;
            fv[k] = f0s[f];
            k++;
        }
        /* simple zero-crossing count of (f0 - mean) */
        double zc = 0;
        for (int i = 1; i < k; i++) {
            if ((fv[i] - m.f0_mean) * (fv[i-1] - m.f0_mean) < 0) zc += 1;
        }
        double span = ts[k-1] - ts[0];
        if (span > 0.3 && zc >= 4) {
            m.vibrato_rate = zc / span / 2.0;   /* cycles per second */
            /* depth: std of f0 relative to mean, in cents */
            double sd = 0;
            for (int i = 0; i < k; i++) { double d = fv[i] - m.f0_mean; sd += d * d; }
            sd = sqrt(sd / k);
            if (m.f0_mean > 0) m.vibrato_depth = 1200.0 * log2(1 + sd / m.f0_mean);
        }
        free(ts); free(fv);
    }

    free(f0s); free(voiced);
    return m;
}

/* ---------------- Formants: LPC + peak-pick + BW (A09-A13) ---------------- */

static double lpc_gain_at(const double *a, int p, double f, int sr) {
    double w = 2.0 * M_PI * f / sr;
    double re = 1.0, im = 0.0;
    double zr = cos(-w), zi = sin(-w);
    double pr = 1.0, pi = 0.0;
    for (int k = 0; k < p; k++) {
        double npr = pr * zr - pi * zi;
        double npi = pr * zi + pi * zr;
        pr = npr; pi = npi;
        re -= a[k] * pr;
        im -= a[k] * pi;
    }
    double denom = re * re + im * im;
    return denom > 1e-12 ? 1.0 / sqrt(denom) : 0.0;
}

wb_formant_measure_t wb_measure_formants(const double *x, size_t n, int sr) {
    wb_formant_measure_t m;
    memset(&m, 0, sizeof(m));

    /* 30 ms window, order sr/1000+4 (rule of thumb) */
    int p = sr / 1000 + 4;
    if (p < 16) p = 16;
    if (p > 40) p = 40;
    size_t win = (size_t)(sr * 0.030);
    if (win > n) win = n;
    size_t start = n / 2 - win / 2;

    double *R = malloc(((size_t)p + 1) * sizeof(double));
    double *coef = calloc((size_t)p, sizeof(double));
    double *w = malloc(win * sizeof(double));
    if (!R || !coef || !w) { free(R); free(coef); free(w); return m; }

    for (size_t i = 0; i < win; i++) {
        double pre = x[start + i] - (i > 0 ? 0.97 * x[start + i - 1] : 0);
        double hann = 0.5 * (1 - cos(2 * M_PI * i / (win - 1)));
        w[i] = pre * hann;
    }
    wb_acorr(w, win, R, (size_t)p);
    double E = 0;
    if (wb_lpc(R, p, coef, &E) != 0) { free(R); free(coef); free(w); return m; }

    /* fine scan 50..4000 Hz in 5 Hz steps */
    double f_lo = 50.0, f_hi = 4000.0;
    if (f_hi > sr / 2.0) f_hi = sr / 2.0;
    int step = 5;
    int nsteps = (int)((f_hi - f_lo) / step);
    double *g = malloc((size_t)nsteps * sizeof(double));
    if (!g) { free(R); free(coef); free(w); return m; }
    for (int i = 0; i < nsteps; i++) g[i] = lpc_gain_at(coef, p, f_lo + i * step, sr);

    /* local maxima, with prominence threshold (20% of global max) so weak
     * spurious peaks don't shadow real formants */
    double gmax = 0;
    for (int i = 0; i < nsteps; i++) if (g[i] > gmax) gmax = g[i];
    double prom = gmax * 0.20;
    double peaks[8], freqs[8];
    int np = 0;
    for (int i = 1; i < nsteps - 1 && np < 8; i++) {
        if (g[i] >= g[i-1] && g[i] > g[i+1] && g[i] > prom) {
            /* parabolic interpolation for sub-5Hz precision */
            double a = g[i-1], b = g[i], c = g[i+1];
            double den = a - 2*b + c;
            double off = den != 0 ? 0.5 * (a - c) / den : 0.0;
            freqs[np] = f_lo + (i + off) * step;
            peaks[np] = b;
            np++;
        }
    }

    /* first 4 peaks are F1..F4; bandwidth via -3dB around each peak */
    for (int k = 0; k < np && k < 4; k++) {
        m.F[k] = freqs[k];
        /* find -3dB points around peak (bounds-checked) */
        double target = peaks[k] / sqrt(2.0);
        int pi = (int)((freqs[k] - f_lo) / step);
        if (pi < 0) pi = 0;
        if (pi >= nsteps) pi = nsteps - 1;
        int lo = pi, hi = pi;
        while (lo > 0 && g[lo] > target) lo--;
        while (hi < nsteps - 1 && g[hi] > target) hi++;
        m.BW[k] = (double)(hi - lo) * step;
        m.n = k + 1;
    }

    free(g); free(R); free(coef); free(w);
    return m;
}

/* ---------------- Quality: jitter/shimmer/HNR/CPP/tilt (A16-A21) ---------------- */

/* Voiced-region extraction: use the frame F0 confidence to build a voiced
 * mask, then find CONTIGUOUS voiced segments (with margin smoothing). We
 * return segments, not a stitched buffer — stitching concatenated frames
 * creates artificial discontinuities that fake zero-crossings and poison
 * jitter/shimmer. */
typedef struct { size_t start; size_t len; } wb_seg_t;

static size_t extract_voiced_segments(const double *x, size_t n, int sr,
                                      wb_seg_t *segs, size_t max_segs) {
    (void)x;
    /* frame-level voiced mask */
    size_t frame = (size_t)(sr * 0.030);
    if (frame < 4) frame = 4;
    size_t hop = frame / 2;
    size_t nframes = n > frame ? 1 + (n - frame) / hop : 1;

    unsigned char *mask = calloc(nframes, 1);
    if (!mask) return 0;

    for (size_t f = 0; f < nframes; f++) {
        size_t start = f * hop;
        if (start + frame > n) start = n - frame;
        double f0 = wb_yin_f0(x + start, frame, sr);
        /* confidence via normalized autocorrelation peak */
        size_t maxlag = (size_t)(sr / 50.0);
        if (maxlag > frame) maxlag = frame;
        double *R = malloc((maxlag + 1) * sizeof(double));
        double conf = 0;
        if (R) {
            wb_acorr(x + start, frame, R, maxlag);
            size_t tmin = (size_t)(sr / 500.0), tmax = maxlag;
            size_t best = tmin;
            for (size_t t = tmin + 1; t <= tmax; t++) if (R[t] > R[best]) best = t;
            if (R[0] > 0) conf = R[best] / R[0];
            free(R);
        }
        mask[f] = (f0 > 40 && f0 < 500 && conf > 0.35) ? 1 : 0;
    }

    /* margin smoothing: a frame is voiced if it or either neighbor is */
    unsigned char *vmask = calloc(nframes, 1);
    if (!vmask) { free(mask); return 0; }
    for (size_t f = 0; f < nframes; f++) {
        if (mask[f]) {
            vmask[f] = 1;
            if (f > 0) vmask[f-1] = 1;
            if (f + 1 < nframes) vmask[f+1] = 1;
        }
    }
    free(mask);

    /* contiguous runs of voiced frames -> sample segments */
    size_t nsegs = 0;
    size_t f = 0;
    while (f < nframes && nsegs < max_segs) {
        if (vmask[f]) {
            size_t fend = f;
            while (fend + 1 < nframes && vmask[fend + 1]) fend++;
            size_t s = f * hop;
            size_t e = (fend + 1) * hop + frame;
            if (e > n) e = n;
            if (e > s) {
                segs[nsegs].start = s;
                segs[nsegs].len = e - s;
                nsegs++;
            }
            f = fend + 1;
        } else {
            f++;
        }
    }
    free(vmask);
    return nsegs;
}

static void pitch_marks(const double *x, size_t n, int sr,
                        size_t **marks_out, double **amps_out, size_t *nm_out) {
    *marks_out = NULL; *amps_out = NULL; *nm_out = 0;
    size_t min_period = (size_t)(sr / 500.0);
    size_t max_period = (size_t)(sr / 50.0);
    if (max_period >= n) max_period = n / 2;
    if (max_period < min_period + 2) return;

    /* pitch-synchronous zero-crossing detection: each glottal cycle has
     * one positive-going zero crossing; finding those is invariant to the
     * LF waveform's lobe shape (which confused peak-picking with
     * double-period intervals). We step by the YIN period and refine to
     * the nearest positive-going zero crossing. */
    double f0_hint = wb_yin_f0(x, n, sr);
    size_t period = f0_hint > 0 ? (size_t)(sr / f0_hint) : (min_period + max_period) / 2;
    if (period < min_period) period = min_period;
    if (period > max_period) period = max_period;

    size_t *marks = malloc(n * sizeof(size_t));
    double *amps = malloc(n * sizeof(double));
    if (!marks || !amps) { free(marks); free(amps); return; }
    size_t nm = 0;

    /* find the first positive-going zero crossing */
    size_t zc = 0;
    for (size_t k = 1; k < n; k++) {
        if (x[k-1] <= 0.0 && x[k] > 0.0) { zc = k; break; }
    }
    if (zc == 0) { free(marks); free(amps); return; }

    size_t best = zc;
    /* advance one full period before collecting: the very first crossing
     * can be a half-cycle artifact (the buffer starts mid-waveform), and
     * its interval with the next mark would poison jitter */
    {
        size_t lo0 = best + period > period / 2 ? best + period - period / 2 : 0;
        size_t hi0 = best + period + period / 2;
        if (hi0 >= n) hi0 = n - 1;
        for (size_t k = lo0; k < hi0 && k + 1 < n; k++) {
            if (x[k] <= 0.0 && x[k+1] > 0.0) { best = k + 1; break; }
        }
    }
    while (best + period < n && nm < n / 2) {
        marks[nm] = best;
        /* amplitude = max |x| within this cycle */
        double amp = 0;
        size_t lo = best, hi = best + period;
        if (hi >= n) hi = n - 1;
        for (size_t k = lo; k <= hi; k++) {
            double v = fabs(x[k]);
            if (v > amp) amp = v;
        }
        amps[nm] = amp;
        nm++;
        /* find the positive-going zero crossing nearest to best+period */
        size_t lo2 = best + period > period / 2 ? best + period - period / 2 : 0;
        size_t hi2 = best + period + period / 2;
        if (hi2 >= n) hi2 = n - 1;
        size_t found = 0;
        for (size_t k = lo2; k < hi2 && k + 1 < n; k++) {
            if (x[k] <= 0.0 && x[k+1] > 0.0) { best = k + 1; found = 1; break; }
        }
        if (!found) break;
    }
    if (nm < 3) { free(marks); free(amps); return; }
    *marks_out = marks; *amps_out = amps; *nm_out = nm;
}

wb_quality_measure_t wb_measure_quality(const double *x, size_t n, int sr) {
    wb_quality_measure_t m;
    memset(&m, 0, sizeof(m));

    /* Find contiguous voiced segments; measure jitter/shimmer on each
     * segment (in place — no stitching, so no fake discontinuities) and
     * length-average. Unvoiced consonants have no glottal pulses and would
     * corrupt the perturbation stats. */
    wb_seg_t segs[64];
    size_t nsegs = extract_voiced_segments(x, n, sr, segs, 64);

    double tot_len = 0;
    for (size_t s = 0; s < nsegs; s++) tot_len += (double)segs[s].len;
    m.voiced_fraction = n > 0 ? tot_len / n : 1.0;

    double jw = 0, sw = 0;
    for (size_t s = 0; s < nsegs; s++) {
        const double *sx = x + segs[s].start;
        size_t sn = segs[s].len;
        size_t *marks; double *amps; size_t nm;
        pitch_marks(sx, sn, sr, &marks, &amps, &nm);
        if (marks && nm >= 3) {
            /* jitter (local %): mean |T_i - T_{i-1}| / mean T * 100 */
            double meanT = 0;
            for (size_t k = 0; k + 1 < nm; k++) meanT += (double)(marks[k+1] - marks[k]);
            meanT /= (double)(nm - 1);
            double jacc = 0;
            for (size_t k = 1; k + 1 < nm; k++) jacc += fabs((double)(marks[k+1] - marks[k]) - (double)(marks[k] - marks[k-1]));
            double jit = meanT > 0 ? jacc / (double)(nm - 2) / meanT * 100.0 : 0.0;

            /* shimmer (local %): mean |A_i - A_{i-1}| / mean A * 100 */
            double meanA = 0;
            for (size_t k = 0; k < nm; k++) meanA += amps[k];
            meanA /= (double)nm;
            double sacc = 0;
            for (size_t k = 1; k < nm; k++) sacc += fabs(amps[k] - amps[k-1]);
            double shim = meanA > 0 ? sacc / (double)(nm - 1) / meanA * 100.0 : 0.0;

            jw += jit * (double)sn;
            sw += shim * (double)sn;
        }
        free(marks); free(amps);
    }
    if (tot_len > 0) {
        m.jitter_pct = jw / tot_len;
        m.shimmer_pct = sw / tot_len;
    }

    /* HNR via autocorrelation (Boersma) — use the whole voiced energy */
    {
        size_t maxlag = (size_t)(sr / 50.0);
        if (maxlag > n) maxlag = n;
        double *R = malloc((maxlag + 1) * sizeof(double));
        if (R) {
            wb_acorr(x, n, R, maxlag);
            size_t tmin = (size_t)(sr / 500.0), tmax = maxlag;
            if (tmax > n) tmax = n;
            size_t best = tmin;
            for (size_t t = tmin + 1; t <= tmax; t++) if (R[t] > R[best]) best = t;
            if (R[0] > R[best] && R[0] > 0 && R[best] >= 0)
                m.hnr_db = 10.0 * log10(R[best] / (R[0] - R[best]));
            free(R);
        }
    }

    /* CPP: cepstral peak prominence (A20) */
    m.cpp = wb_measure_cpp(x, n, sr);

    /* spectral tilt (A16): slope of log-magnitude spectrum in dB/octave */
    {
        size_t N = 2048;
        if (n < N) N = 1; while (N < n && N < 65536) N <<= 1;
        if (N >= 2) {
            double *win = malloc(N * sizeof(double));
            double *Re = malloc(N * sizeof(double));   /* rfft needs N work space */
            double *Im = malloc(N * sizeof(double));
            if (win && Re && Im) {
                size_t start = n / 2 - N / 2;
                for (size_t i = 0; i < N; i++) {
                    double hann = 0.5 * (1 - cos(2 * M_PI * i / (N - 1)));
                    win[i] = x[start + i] * hann;
                }
                if (wb_rfft(win, N, Re, Im) == 0) {
                    /* fit dB vs log2(f) over 200..4000 Hz */
                    double sx = 0, sy = 0, sxx = 0, sxy = 0, cnt = 0;
                    for (size_t i = 1; i <= N / 2; i++) {
                        double f = (double)i * sr / N;
                        if (f < 200 || f > 4000) continue;
                        double mag = sqrt(Re[i] * Re[i] + Im[i] * Im[i]);
                        double db = 20.0 * log10(mag > 1e-12 ? mag : 1e-12);
                        double xv = log2(f);
                        sx += xv; sy += db; sxx += xv * xv; sxy += xv * db; cnt++;
                    }
                    if (cnt > 2) {
                        double den = cnt * sxx - sx * sx;
                        if (den != 0) m.tilt_db_per_oct = (cnt * sxy - sx * sy) / den;
                    }
                }
                free(win); free(Re); free(Im);
            }
        }
    }

    /* H1-H2 (A21): difference of first two harmonic amplitudes */
    {
        size_t N = 4096;
        if (n < N) N = 1; while (N < n && N < 65536) N <<= 1;
        if (N >= 2) {
            double *win = malloc(N * sizeof(double));
            double *Re = malloc(N * sizeof(double));   /* rfft needs N work space */
            double *Im = malloc(N * sizeof(double));
            if (win && Re && Im) {
                size_t start = n / 2 - N / 2;
                for (size_t i = 0; i < N; i++) {
                    double hann = 0.5 * (1 - cos(2 * M_PI * i / (N - 1)));
                    win[i] = x[start + i] * hann;
                }
                if (wb_rfft(win, N, Re, Im) == 0) {
                    double f0 = wb_yin_f0(x, n, sr);
                    if (f0 > 40 && f0 < 500) {
                        double h1 = 0, h2 = 0;
                        int b1 = (int)(f0 * N / sr), b2 = (int)(2 * f0 * N / sr);
                        if (b1 > 0 && b1 < (int)(N / 2)) h1 = sqrt(Re[b1]*Re[b1] + Im[b1]*Im[b1]);
                        if (b2 > 0 && b2 < (int)(N / 2)) h2 = sqrt(Re[b2]*Re[b2] + Im[b2]*Im[b2]);
                        if (h1 > 1e-12 && h2 > 1e-12)
                            m.h1h2_db = 20.0 * log10(h1 / h2);
                    }
                }
                free(win); free(Re); free(Im);
            }
        }
    }

    return m;
}

/* Proper CPP via real cepstrum (A20) */
double wb_measure_cpp(const double *x, size_t n, int sr) {
    (void)sr;
    enum { SR_CPP = 44100 };
    size_t N = 2048;
    if (n < N) N = 1; while (N < n && N < 65536) N <<= 1;
    if (N < 2) return 0.0;
    double *win = malloc(N * sizeof(double));
    double *Re = malloc(N * sizeof(double));   /* rfft needs N work space */
    double *Im = malloc(N * sizeof(double));
    if (!win || !Re || !Im) { free(win); free(Re); free(Im); return 0.0; }
    size_t start = n / 2 - N / 2;
    for (size_t i = 0; i < N; i++) {
        double hann = 0.5 * (1 - cos(2 * M_PI * i / (N - 1)));
        win[i] = x[start + i] * hann;
    }
    if (wb_rfft(win, N, Re, Im) != 0) { free(win); free(Re); free(Im); return 0.0; }

    /* log magnitude spectrum, packed as real array for the inverse FFT.
     * We compute the real cepstrum: c[n] = IFFT(log|X|). */
    double *ls = malloc(N * sizeof(double));
    for (size_t i = 0; i < N; i++) {
        size_t idx = i <= N / 2 ? i : N - i;
        double mag = sqrt(Re[idx] * Re[idx] + Im[idx] * Im[idx]);
        ls[i] = mag > 1e-12 ? log(mag) : -20.0;
    }
    /* inverse FFT: forward with conjugated spectrum then scale by 1/N */
    /* (we compute the cepstral peak directly in the quefrency loop below) */
    /* CPP: cepstral peak prominence (A20).
     * Only scan the pitch-relevant quefrency range (2ms..20ms => 50-500Hz
     * F0), which is both faster and more correct than scanning everything. */
    double cpp = 0.0;
    {
        size_t q_min = (size_t)(SR_CPP / 500.0);   /* 500 Hz -> 88 bins @44.1k */
        size_t q_max = (size_t)(SR_CPP / 50.0);    /* 50 Hz  -> 882 bins */
        if (q_min < 1) q_min = 1;
        if (q_max >= N / 2) q_max = N / 2 - 1;
        double maxc = -1e18;
        for (size_t q = q_min; q <= q_max; q++) {
            double acc = 0;
            for (size_t k = 0; k < N; k++) {
                double ang = 2.0 * M_PI * (double)(q * k) / N;
                acc += ls[k] * cos(ang);   /* real part of IFFT (1/N outside) */
            }
            acc /= N;
            if (acc > maxc) maxc = acc;
        }
        /* prominence = peak minus the cepstral mean (a simple baseline) */
        double mean = 0;
        for (size_t k = 0; k < N; k++) mean += ls[k];
        mean /= N;
        cpp = maxc - mean;
    }
    free(ls); free(win); free(Re); free(Im);
    return cpp;
}
