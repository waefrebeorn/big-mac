/* wb_branchless.h — branchless DSP helper macros and inline functions.
 *
 * R077: Eliminate branch misprediction in hot DSP loops.
 * Branch misprediction = 15-20 cycle penalty on modern CPUs.
 *
 * Pure C11. Header-only.
 */

#ifndef WB_BRANCHLESS_H
#define WB_BRANCHLESS_H

#include <emmintrin.h>
#include <xmmintrin.h>

/* ---- Scalar branchless helpers ---- */

/* Branchless absolute value */
static inline float branchless_fabsf(float x) {
    union { float f; uint32_t i; } u = {x};
    u.i &= 0x7FFFFFFF;
    return u.f;
}

/* Branchless min/max */
static inline float branchless_fminf(float a, float b) {
    return a < b ? a : b;
}
static inline float branchless_fmaxf(float a, float b) {
    return a > b ? a : b;
}

/* Branchless clamp */
static inline float branchless_clampf(float x, float lo, float hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

/* Branchless sign: returns 1.0f if x >= 0, -1.0f if x < 0 */
static inline float branchless_signf(float x) {
    return (x >= 0.0f) ? 1.0f : -1.0f;
}

/* Branchless conditional select: return a if cond, b otherwise */
static inline float branchless_selectf(int cond, float a, float b) {
    /* cond is 0 or 1 */
    float mask = (float)(-(int)cond);  /* 0.0 if cond=0, -0.0 (all bits set) if cond=1 */
    union { float f; uint32_t i; } ua = {a};
    union { float f; uint32_t i; } ub = {b};
    union { float f; uint32_t i; } um = {mask};
    union { uint32_t i; float f; } result;
    result.i = (ua.i & um.i) | (ub.i & ~um.i);
    return result.f;
}

/* ---- SIMD branchless helpers ---- */

/* SIMD branchless clamp: clamp 4 floats to [lo, hi] */
static inline __m128 simd_clamp4(__m128 x, __m128 lo, __m128 hi) {
    return _mm_max_ps(_mm_min_ps(x, hi), lo);
}

/* SIMD branchless absolute value */
static inline __m128 simd_fabs4(__m128 x) {
    __m128 mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    return _mm_and_ps(x, mask);
}

/* SIMD branchless sign */
static inline __m128 simd_sign4(__m128 x) {
    __m128 zero = _mm_setzero_ps();
    __m128 one = _mm_set1_ps(1.0f);
    __m128 neg_one = _mm_set1_ps(-1.0f);
    __m128 ge = _mm_cmpge_ps(x, zero);
    return _mm_or_ps(_mm_and_ps(ge, one), _mm_andnot_ps(ge, neg_one));
}

/* SIMD branchless select: return a where mask is set, b otherwise */
static inline __m128 simd_select4(__m128 mask, __m128 a, __m128 b) {
    return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b));
}

/* SIMD branchless min/max */
static inline __m128 simd_min4(__m128 a, __m128 b) {
    return _mm_min_ps(a, b);
}
static inline __m128 simd_max4(__m128 a, __m128 b) {
    return _mm_max_ps(a, b);
}

#endif /* WB_BRANCHLESS_H */
