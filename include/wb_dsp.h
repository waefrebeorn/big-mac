/*
 * wb_dsp.h — Big Mac DSP primitives (analysis side, strict C11, no third party)
 *
 * The measurement layer: everything needed to ABSORB a vocal demo —
 * autocorrelation, LPC (Levinson-Durbin), FFT, YIN pitch, jitter/shimmer.
 */
#ifndef WB_DSP_H
#define WB_DSP_H

#include <stddef.h>

/* ---------- autocorrelation ---------- */
/* R[k] = sum_n x[n]*x[n+k] for k=0..maxlag. x has n samples. */
void wb_acorr(const double *x, size_t n, double *R, size_t maxlag);

/* ---------- LPC (Levinson-Durbin) ---------- */
/* Solve order-p LPC from autocorrelation R[0..p]. Returns prediction
 * coefficients a[0..p-1] (a[0]=1 convention NOT used; these are the
 * direct filter coefficients) and the residual energy E.
 * Returns 0 on success, -1 if matrix singular. */
int wb_lpc(const double *R, int p, double *a, double *E);

/* ---------- FFT (radix-2, in place, real input) ---------- */
/* Forward FFT: x is n real samples (n must be power of 2, n>=2).
 * Output: Re[0..n/2], Im[0..n/2] (the two halves of the spectrum).
 * Re/Im arrays must have n/2+1 entries. Returns 0 on success. */
int wb_rfft(const double *x, size_t n, double *Re, double *Im);

/* ---------- YIN pitch (simplified) ---------- */
/* Estimate F0 (Hz) of a window of samples at given sample rate.
 * Returns F0 or 0.0 if unvoiced. */
double wb_yin_f0(const double *x, size_t n, int sample_rate);

/* ---------- peak picking ---------- */
/* Find the index of the maximum of |x| in [lo, hi]. */
size_t wb_argmax_abs(const double *x, size_t lo, size_t hi);

#endif /* WB_DSP_H */
