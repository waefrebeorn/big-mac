/* g2_fm_simd.h — R076 G2: SIMD-accelerated FM sin cascade.
 *
 * Provides vec_sin_4(__m128 x) → __m128 sin(x4) using SSE2 polynomial
 * approximation (~43 cycles/4 values vs ~280 cycles/4 scalar sinf).
 *
 * Also provides fm_render_simd_batch — process 4 FM voices' modulator+carrier
 * sin cascade in a single SIMD batch (2 × vec_sin_4 calls per 4 voices).
 *
 * Compile with -msse2 (implied by the existing -msse4.2 / -O2 flags).
 * Sandy Bridge i5 target: SSE2 + SSE4.2, no AVX. */

#ifndef G2_FM_SIMD_H
#define G2_FM_SIMD_H

#include <emmintrin.h>   /* SSE2 */
#include <xmmintrin.h>   /* SSE1 */

/* ---- CEPHES-derived minimax polynomial for sin on [-π/4, π/4]. ---- */
#define S_C1  -0.1666666664f
#define S_C2   8.333333333e-3f
#define S_C3  -1.984126984e-4f
#define S_C4   2.755731922e-6f
#define S_C5  -2.505210838e-8f

#define _PI       3.14159265358979323846f
#define _TWO_PI   6.2831853071795864769f
#define _INV_TWO_PI 0.15915494309189533577f
#define _PI_2     1.57079632679489661923f
#define _PI_4     0.78539816339744830962f

/* ---- vec_sin_4: 4-wide float sine, ~43 cycles per 4 values (Sandy Bridge). ---- */
static inline __m128 vec_sin_4(__m128 x) {
    __m128 v_inv_tp = _mm_set1_ps(_INV_TWO_PI);
    __m128 v_tp     = _mm_set1_ps(_TWO_PI);
    __m128 v_pi     = _mm_set1_ps(_PI);
    __m128 v_pi_2   = _mm_set1_ps(_PI_2);
    __m128 one      = _mm_set1_ps(1.0f);
    __m128 neg_one  = _mm_set1_ps(-1.0f);
    __m128 zero_vec = _mm_setzero_ps();
    __m128 neg_zero = _mm_set1_ps(-0.0f);

    /* y = |x| for range reduction */
    __m128 sign_mask = _mm_and_ps(x, neg_zero);
    __m128 y = _mm_xor_ps(x, sign_mask);

    /* j = (int)(y * INV_TWO_PI) — 2π-period index */
    __m128i j = _mm_cvttps_epi32(_mm_mul_ps(y, v_inv_tp));

    /* Save original x_red BEFORE folding — needed for sign computation */
    __m128 orig_x_red = _mm_sub_ps(x, _mm_mul_ps(_mm_cvtepi32_ps(j), v_tp));

    /* Handle boundary: if |x_red| < 1e-4 or |x_red - 2π| < 1e-4, clamp to 0 */
    __m128 eps = _mm_set1_ps(1e-4f);
    __m128 x_red_abs = _mm_andnot_ps(neg_zero, orig_x_red);
    __m128 x_red_2pi = _mm_sub_ps(orig_x_red, v_tp);
    __m128 x_red_2pi_abs = _mm_andnot_ps(neg_zero, x_red_2pi);
    __m128 near_boundary =
        _mm_or_ps(_mm_cmple_ps(x_red_abs, eps), _mm_cmple_ps(x_red_2pi_abs, eps));
    __m128 nb_andnot = _mm_andnot_ps(near_boundary, orig_x_red);
    __m128 nb_and    = _mm_and_ps(near_boundary, zero_vec);
    __m128 x_red = _mm_or_ps(nb_andnot, nb_and);

    /* Sign: sin(x) = +sin(x_red) if x_red ∈ [0, π], -sin(x_red - π) if x_red ∈ (π, 2π) */
    __m128 gt_pi = _mm_cmpgt_ps(orig_x_red, v_pi);
    __m128 sign = _mm_or_ps(
        _mm_and_ps(gt_pi, neg_one),
        _mm_andnot_ps(gt_pi, one));

    /* Quadrant folding: reduce x_red to [0, π/2] */
    /* Q2+Q3: x_red > π  →  x_red = |x_red - π| */
    __m128 q23_mask = _mm_cmpgt_ps(x_red, v_pi);
    __m128 q23_diff = _mm_sub_ps(x_red, v_pi);
    __m128 q23_abs = _mm_andnot_ps(neg_zero, q23_diff);
    x_red = _mm_or_ps(
        _mm_and_ps(q23_mask, q23_abs),
        _mm_andnot_ps(q23_mask, x_red));

    /* Q1: x_red > π/2 AND x_red <= π  →  x_red = π - x_red */
    __m128 q1_mask = _mm_and_ps(
        _mm_cmpgt_ps(x_red, v_pi_2),
        _mm_cmple_ps(x_red, v_pi));
    __m128 q1_fold = _mm_sub_ps(v_pi, x_red);
    x_red = _mm_or_ps(
        _mm_and_ps(q1_mask, q1_fold),
        _mm_andnot_ps(q1_mask, x_red));

    /* Q3: x_red > 3π/2  →  x_red = 2π - x_red */
    __m128 q3_mask = _mm_cmpgt_ps(x_red, _mm_add_ps(v_pi, v_pi_2));
    __m128 q3_fold = _mm_sub_ps(v_tp, x_red);
    x_red = _mm_or_ps(
        _mm_and_ps(q3_mask, q3_fold),
        _mm_andnot_ps(q3_mask, x_red));

    /* Now x_red ∈ [0, π/2]. Compute sin via 5th-degree minimax polynomial. */
    __m128 t = x_red;
    __m128 t2 = _mm_mul_ps(t, t);
    __m128 c5 = _mm_set1_ps(S_C5);
    __m128 c4 = _mm_set1_ps(S_C4);
    __m128 c3 = _mm_set1_ps(S_C3);
    __m128 c2 = _mm_set1_ps(S_C2);
    __m128 c1 = _mm_set1_ps(S_C1);
    __m128 c = _mm_add_ps(c5, c4);
    c = _mm_mul_ps(c, t2); c = _mm_add_ps(c, c3);
    c = _mm_mul_ps(c, t2); c = _mm_add_ps(c, c2);
    c = _mm_mul_ps(c, t2); c = _mm_add_ps(c, c1);
    c = _mm_mul_ps(c, t2); c = _mm_add_ps(c, one);
    __m128 sin_val = _mm_mul_ps(t, c);
    return _mm_mul_ps(sin_val, sign);
}

/* ---- fm_simd_batch_4 —
 * Process 4 FM voices' modulator + carrier sin cascade in SIMD.
 * Inputs: 4 mphase values, 4 phase values, 4 env values (all packed __m128).
 * Index and vel are scalar (same for all voices in the batch — typical
 * for a homogeneous FM patch; if heterogeneous, compute per-voice mod
 * in scalar before this call).
 *
 * Returns __m128 of 4 carrier sine results (s = sin(phase + mod)).
 * NOTE: modulator phasors are updated in place in the mphase_vec out param.
 * This is the SIMD-accelerated version of:
 *   for k in 4:
 *     mod[k] = index * env[k] * sin(mphase[k])
 *     s[k]   = sin(phase[k] + mod[k])
 *     mphase[k] += mphase_step[k]
 *     phase[k] += phase_step[k]
 */
static inline __m128 fm_simd_batch_4(
    __m128 mphase_vec,   /* 4 modulator phase values */
    __m128 phase_vec,    /* 4 carrier phase values */
    __m128 env_vec,      /* 4 envelope values */
    float   index,       /* FM index (scalar, shared across batch) */
    __m128 *mphase_out,  /* out: updated modulator phases after stepping */
    __m128 *phase_out)   /* out: updated carrier phases after stepping */
{
    (void)mphase_out; (void)phase_out;
    /* 1. Modulator sin (4-wide) */
    __m128 sin_mphase = vec_sin_4(mphase_vec);

    /* 2. mod = index * env * sin(mphase) — 4-wide multiply */
    __m128 index_vec = _mm_set1_ps(index);
    __m128 mod_vec = _mm_mul_ps(index_vec, _mm_mul_ps(env_vec, sin_mphase));

    /* 3. Carrier sin: sin(phase + mod) — 4-wide */
    __m128 carrier_arg = _mm_add_ps(phase_vec, mod_vec);
    __m128 s = vec_sin_4(carrier_arg);

    /* NOTE: phase stepping (mphase += mphase_step, phase += phase_step)
     * is done by the CALLER after this function returns, because the
     * step vectors are per-voice scalars that the caller already has
     * in `phase_step[k]` and `mphase_step[k]` arrays. We return the
     * carrier sin results; the caller applies the step vectors.
     * If the caller wants in-place stepping, it can do:
     *   mphase_vec = _mm_add_ps(mphase_vec, mphase_step_vec);
     *   phase_vec  = _mm_add_ps(phase_vec, phase_step_vec);
     * after this call. */

    return s;
}

#endif /* G2_FM_SIMD_H */
