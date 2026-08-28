/* wb_mix_simd.c — SIMD mixer bus with constant-power pan laws.
 *
 * Sums N tracks to a master bus using SSE2. Two improvements over the
 * scalar stage_mix:
 *   1. Constant-power (equal-power) pan law: sin/cos taper instead of
 *      linear, so center-panned signals don't sound louder than panned.
 *   2. SIMD master volume + peak/RMS metering: 4 samples/iteration.
 *
 * G7 [R075]: mixer bus channel summing with pan laws.
 * Pure C11, SSE2 intrinsics. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <emmintrin.h>
#include "wbus.h"

/* ---- Constant-power pan law ---- */
/* Equal-power pan: L = cos(θ), R = sin(θ) where θ = (pan+1) * π/4.
 * At pan=0: L=R=√2/2 ≈ 0.707 (same perceived loudness as linear 1.0).
 * At pan=-1: L=1, R=0. At pan=+1: L=0, R=1.
 * This avoids the "center bulge" of linear pan. */

/* SSE2 vectorized pan gains for 4 pan values */
static inline void pan_gains_4(__m128 pan_vec, __m128 *L_out, __m128 *R_out) {
    __m128 half_pi = _mm_set1_ps(0.785398163f);  /* π/4 */
    __m128 one = _mm_set1_ps(1.0f);
    __m128 theta = _mm_mul_ps(_mm_add_ps(pan_vec, one), half_pi);
    /* cos/sin via scalar fallback (no SIMD trig in SSE2) */
    float theta_arr[4], L_arr[4], R_arr[4];
    _mm_storeu_ps(theta_arr, theta);
    for (int j = 0; j < 4; j++) {
        L_arr[j] = cosf(theta_arr[j]);
        R_arr[j] = sinf(theta_arr[j]);
    }
    *L_out = _mm_loadu_ps(L_arr);
    *R_out = _mm_loadu_ps(R_arr);
}

/* ---- SIMD mixer bus ---- */

/* Sum 4 stereo track buffers into master output with per-track volume + pan.
 * All arrays are interleaved stereo: [L0, R0, L1, R1, ...]
 * tracks[4] are pointers to track buffer arrays (interleaved stereo).
 * volumes[4] are per-track linear gains.
 * pans[4] are per-track pan values [-1, 1].
 * Output is interleaved stereo, length n_samples (n_samples*2 floats). */
void wb_mix4_tracks(const wb_sample *tracks[4], const float volumes[4],
                    const float pans[4], wb_sample *output, uint32_t n_samples) {
    /* Compute pan gains for 4 tracks */
    __m128 pan_vec = _mm_loadu_ps(pans);
    __m128 L_gain, R_gain;
    pan_gains_4(pan_vec, &L_gain, &R_gain);

    __m128 vol_vec = _mm_loadu_ps(volumes);
    L_gain = _mm_mul_ps(L_gain, vol_vec);
    R_gain = _mm_mul_ps(R_gain, vol_vec);

    /* Extract per-lane gains for scalar access */
    float Lg[4], Rg[4];
    _mm_storeu_ps(Lg, L_gain);
    _mm_storeu_ps(Rg, R_gain);

    uint32_t i = 0;

    /* SSE2: process 2 stereo samples (4 floats) per iteration */
    for (; i + 1 < n_samples; i += 2) {
        __m128 master = _mm_setzero_ps();
        for (int t = 0; t < 4; t++) {
            /* Load 2 stereo samples from track t */
            __m128 tr = _mm_loadu_ps(&tracks[t][i * 2]);
            /* Apply L gain to even samples, R gain to odd samples */
            /* tr = [L0, R0, L1, R1] */
            /* We need: [L0*Lg, R0*Rg, L1*Lg, R1*Rg] */
            __m128 Lg_vec = _mm_set1_ps(Lg[t]);
            __m128 Rg_vec = _mm_set1_ps(Rg[t]);
            /* Interleave: [Lg, Rg, Lg, Rg] for 2 stereo frames */
            __m128 gain = _mm_unpacklo_ps(Lg_vec, Rg_vec);  /* [Lg, Rg, Lg, Rg] */
            master = _mm_add_ps(master, _mm_mul_ps(tr, gain));
        }
        /* Add to output (accumulate) */
        _mm_storeu_ps(&output[i * 2], _mm_add_ps(_mm_loadu_ps(&output[i * 2]), master));
    }

    /* Tail: scalar */
    for (; i < n_samples; i++) {
        float sumL = 0, sumR = 0;
        for (int t = 0; t < 4; t++) {
            sumL += tracks[t][i * 2]     * Lg[t];
            sumR += tracks[t][i * 2 + 1] * Rg[t];
        }
        output[i * 2]     += sumL;
        output[i * 2 + 1] += sumR;
    }
}

/* SIMD master volume + peak/RMS metering.
 * Processes interleaved stereo, length n_samples*2 floats.
 * Applies gain, returns peak and writes back sumsq for RMS. */
float wb_mix_master_volume(wb_sample *buf, uint32_t n_samples, float gain, float *sumsq) {
    __m128 g = _mm_set1_ps(gain);
    __m128 peak = _mm_setzero_ps();
    __m128 sumsq_vec = _mm_setzero_ps();

    uint32_t total = n_samples * 2;
    uint32_t i = 0;

    for (; i + 3 < total; i += 4) {
        __m128 s = _mm_loadu_ps(&buf[i]);
        s = _mm_mul_ps(s, g);
        _mm_storeu_ps(&buf[i], s);

        __m128 abs_s = _mm_andnot_ps(_mm_set1_ps(-0.0f), s);
        peak = _mm_max_ps(peak, abs_s);
        sumsq_vec = _mm_add_ps(sumsq_vec, _mm_mul_ps(s, s));
    }

    /* Horizontal reduce */
    float peak_arr[4], sumsq_arr[4];
    _mm_storeu_ps(peak_arr, peak);
    _mm_storeu_ps(sumsq_arr, sumsq_vec);
    float pk = 0, ss = 0;
    for (int j = 0; j < 4; j++) {
        if (peak_arr[j] > pk) pk = peak_arr[j];
        ss += sumsq_arr[j];
    }

    /* Tail */
    for (; i < total; i++) {
        buf[i] *= gain;
        float a = fabsf(buf[i]);
        if (a > pk) pk = a;
        ss += buf[i] * buf[i];
    }

    *sumsq = ss;
    return pk;
}
