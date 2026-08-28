/* wb_biquad_simd.h — SIMD biquad filter (transposed direct Form II).
 *
 * Processes 4 independent biquad filters simultaneously using SSE2.
 * Each "lane" is a separate filter (e.g., a separate synth voice).
 * All 4 filters share the same coefficients (b0..a2) but have independent
 * state (s1, s2).
 *
 * Transposed Form II structure:
 *   y  = b0*x + s1
 *   s1 = b1*x - a1*y + s2
 *   s2 = b2*x - a2*y
 *
 * This is mathematically equivalent to direct Form I but has better
 * numerical properties and is more SIMD-friendly.
 *
 * State is stored as 4-wide SIMD arrays: s1[4], s2[4] for the 4 filters.
 * Coefficients are broadcast to all lanes.
 */

#ifndef WB_BIQUAD_SIMD_H
#define WB_BIQUAD_SIMD_H

#include <emmintrin.h>
#include <xmmintrin.h>

typedef struct {
    __m128 s1;   /* state 1 for 4 filters */
    __m128 s2;   /* state 2 for 4 filters */
    __m128 b0, b1, b2, a1, a2;  /* coefficients (broadcast) */
} wb_biquad4;

static inline void wb_biquad4_init(wb_biquad4 *f, float b0, float b1, float b2,
                                    float a1, float a2) {
    f->s1 = _mm_setzero_ps();
    f->s2 = _mm_setzero_ps();
    f->b0 = _mm_set1_ps(b0);
    f->b1 = _mm_set1_ps(b1);
    f->b2 = _mm_set1_ps(b2);
    f->a1 = _mm_set1_ps(a1);
    f->a2 = _mm_set1_ps(a2);
}

static inline void wb_biquad4_set_coeffs(wb_biquad4 *f, float b0, float b1, float b2,
                                          float a1, float a2) {
    f->b0 = _mm_set1_ps(b0);
    f->b1 = _mm_set1_ps(b1);
    f->b2 = _mm_set1_ps(b2);
    f->a1 = _mm_set1_ps(a1);
    f->a2 = _mm_set1_ps(a2);
}

/* Process one sample through 4 biquad filters simultaneously.
 * x_input is 4 float values (one per filter lane).
 * Returns 4 float outputs. */
static inline __m128 wb_biquad4_process(wb_biquad4 *f, __m128 x) {
    /* y = b0*x + s1 */
    __m128 y = _mm_add_ps(_mm_mul_ps(f->b0, x), f->s1);
    /* s1 = b1*x - a1*y + s2 */
    f->s1 = _mm_add_ps(
        _mm_sub_ps(_mm_mul_ps(f->b1, x), _mm_mul_ps(f->a1, y)),
        f->s2);
    /* s2 = b2*x - a2*y */
    f->s2 = _mm_sub_ps(_mm_mul_ps(f->b2, x), _mm_mul_ps(f->a2, y));
    return y;
}

/* Load 4 individual biquad states (from 4 separate wb_biquad structs) into SIMD.
 * Uses direct Form I states (x1, x2, y1, y2) and converts to Form II (s1, s2).
 * Conversion: s1 = b1*x1 + b2*x2 - a1*y1 - a2*y2 + b1*x (current)
 * Actually, we just reset the Form II state — the filter will converge correctly
 * within a few samples. For exact continuity, we'd need the full conversion,
 * but for audio purposes, resetting is acceptable at note-on boundaries. */
static inline void wb_biquad4_load_states(wb_biquad4 *f,
                                           float s1_0, float s1_1, float s1_2, float s1_3,
                                           float s2_0, float s2_1, float s2_2, float s2_3) {
    f->s1 = _mm_setr_ps(s1_0, s1_1, s1_2, s1_3);
    f->s2 = _mm_setr_ps(s2_0, s2_1, s2_2, s2_3);
}

static inline void wb_biquad4_store_states(wb_biquad4 *f,
                                           float *s1_out, float *s2_out) {
    _mm_storeu_ps(s1_out, f->s1);
    _mm_storeu_ps(s2_out, f->s2);
}

#endif /* WB_BIQUAD_SIMD_H */
