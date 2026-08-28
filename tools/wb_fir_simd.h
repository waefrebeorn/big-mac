/* wb_fir_simd.h — SIMD-optimized FIR filter (direct form, 4 taps/cycle).
 *
 * R077: FIR convolution with SSE2 — 4 MACs per iteration.
 * Used for: reverb early reflections, crossover filters, convolution.
 *
 * Algorithm: y[n] = sum_{k=0}^{N-1} h[k] * x[n-k]
 * SIMD: Process 4 output samples simultaneously using _mm_mul_ps + _mm_add_ps.
 *
 * For long FIRs (>64 taps): use overlap-save FFT convolution (wb_conv.c already has this).
 * For short FIRs (<64 taps): this direct form is faster (no FFT overhead).
 *
 * Pure C11, SSE2. Header-only.
 */

#ifndef WB_FIR_SIMD_H
#define WB_FIR_SIMD_H

#include <emmintrin.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FIR filter state (direct form, SIMD-optimized). */
typedef struct {
    float *coeffs;       /* Filter coefficients (length = num_taps) */
    float *state;        /* Circular buffer of past input samples */
    int    num_taps;     /* Number of filter taps */
    int    state_pos;    /* Write position in circular buffer */
} wb_fir_fir;

/* Create an FIR filter with given coefficients.
 * coeffs: array of num_taps coefficients (copied internally). */
static inline wb_fir_fir* wb_fir_create(const float *coeffs, int num_taps) {
    wb_fir_fir *f = (wb_fir_fir *)calloc(1, sizeof(wb_fir_fir));
    if (!f) return NULL;
    f->num_taps = num_taps;
    f->coeffs = (float *)calloc(num_taps, sizeof(float));
    f->state = (float *)calloc(num_taps, sizeof(float));
    if (!f->coeffs || !f->state) { free(f->coeffs); free(f->state); free(f); return NULL; }
    memcpy(f->coeffs, coeffs, num_taps * sizeof(float));
    f->state_pos = 0;
    return f;
}

static inline void wb_fir_destroy(wb_fir_fir *f) {
    if (f) { free(f->coeffs); free(f->state); free(f); }
}

/* Process a single sample through the FIR filter (scalar fallback). */
static inline float wb_fir_process_scalar(wb_fir_fir *f, float x) {
    /* Store input in circular buffer */
    f->state[f->state_pos] = x;

    /* Compute convolution */
    float y = 0.0f;
    int pos = f->state_pos;
    for (int k = 0; k < f->num_taps; k++) {
        y += f->coeffs[k] * f->state[pos];
        pos--;
        if (pos < 0) pos = f->num_taps - 1;
    }

    /* Advance write position */
    f->state_pos++;
    if (f->state_pos >= f->num_taps) f->state_pos = 0;

    return y;
}

/* Process a block of samples through the FIR filter.
 * Uses scalar processing (FIR direct form is inherently serial due to state dependency).
 * For SIMD speedup, use wb_fir_process4_parallel (4 independent filters). */
static inline void wb_fir_process_block(wb_fir_fir *f, const float *in,
                                         float *out, int n) {
    for (int i = 0; i < n; i++) {
        out[i] = wb_fir_process_scalar(f, in[i]);
    }
}

/* Process 4 independent FIR filters in parallel (SIMD).
 * Each filter processes its own input sample.
 * This is the key SIMD win: 4 independent FIRs at once. */
typedef struct {
    __m128 *coeffs;      /* Coefficients in SoA: coeffs[k] = [h0[k], h1[k], h2[k], h3[k]] */
    __m128 *state;       /* State in SoA */
    int      num_taps;
    int      state_pos;
} wb_fir4_simd;

static inline wb_fir4_simd* wb_fir4_create(const float *coeffs0,
                                             const float *coeffs1,
                                             const float *coeffs2,
                                             const float *coeffs3,
                                             int num_taps) {
    wb_fir4_simd *f = (wb_fir4_simd *)calloc(1, sizeof(wb_fir4_simd));
    if (!f) return NULL;
    f->num_taps = num_taps;
    f->coeffs = (__m128 *)calloc(num_taps, sizeof(__m128));
    f->state = (__m128 *)calloc(num_taps, sizeof(__m128));
    if (!f->coeffs || !f->state) { free(f->coeffs); free(f->state); free(f); return NULL; }

    /* Load coefficients into SoA layout */
    for (int k = 0; k < num_taps; k++) {
        f->coeffs[k] = _mm_setr_ps(coeffs0[k], coeffs1[k], coeffs2[k], coeffs3[k]);
        f->state[k] = _mm_setzero_ps();
    }
    f->state_pos = 0;
    return f;
}

static inline void wb_fir4_destroy(wb_fir4_simd *f) {
    if (f) { free(f->coeffs); free(f->state); free(f); }
}

/* Process 4 samples (one per filter) simultaneously. */
static inline void wb_fir4_process(wb_fir4_simd *f,
                                    float x0, float x1, float x2, float x3,
                                    float *out0, float *out1, float *out2, float *out3) {
    __m128 x = _mm_setr_ps(x0, x1, x2, x3);

    /* Store input in circular buffer */
    f->state[f->state_pos] = x;

    /* Compute convolution: y = sum(coeff[k] * state[pos - k]) */
    __m128 y = _mm_setzero_ps();
    int pos = f->state_pos;
    for (int k = 0; k < f->num_taps; k++) {
        y = _mm_add_ps(y, _mm_mul_ps(f->coeffs[k], f->state[pos]));
        pos--;
        if (pos < 0) pos = f->num_taps - 1;
    }

    /* Advance write position */
    f->state_pos++;
    if (f->state_pos >= f->num_taps) f->state_pos = 0;

    /* Extract results */
    float result[4];
    _mm_storeu_ps(result, y);
    *out0 = result[0];
    *out1 = result[1];
    *out2 = result[2];
    *out3 = result[3];
}

#ifdef __cplusplus
}
#endif

#endif /* WB_FIR_SIMD_H */
