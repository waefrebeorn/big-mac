#ifndef WBUS_WBUS_FFT_H
#define WBUS_WBUS_FFT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal self-contained radix-2 Cooley-Tukey FFT (no third-party deps).
 * R018-D: powers the spectral voice-isolation gate. Sizes must be a power
 * of two (caller zero-pads). Real-input helper fills re[]/im[] and runs the
 * complex transform; magnitude helpers recover the spectrum. */

typedef struct wb_fft_plan {
    int n;              /* transform size (power of two) */
    int *rev;           /* bit-reversal indices */
} wb_fft_plan;

/* Create a plan for size n (must be power of two, 2..65536). Returns NULL
 * on bad size. Free with wb_fft_destroy. */
wb_fft_plan *wb_fft_create(int n);
void         wb_fft_destroy(wb_fft_plan *p);

/* In-place complex FFT. re/im are length n. invert!=0 computes IFFT
 * (unnormalized; caller divides by n for true inverse). */
void wb_fft_run(const wb_fft_plan *p, double *re, double *im, int invert);

/* Convenience: real-input forward FFT. Fills re[] with the signal, im[]=0,
 * runs the transform. Output is conjugate-symmetric (use bins 0..n/2). */
void wb_fft_real(const wb_fft_plan *p, const double *x, double *re, double *im);

/* Inverse of wb_fft_real: overlap-free ISTFT done by caller; this returns
 * the time signal from a (possibly modified) spectrum. out length n. */
void wb_fft_real_inverse(const wb_fft_plan *p, const double *re, const double *im,
                         double *out);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_WBUS_FFT_H */
