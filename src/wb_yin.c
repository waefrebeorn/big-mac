/* wb_yin.c — YIN pitch detection algorithm.
 *
 * N2 [R076]: real-time pitch tracking for tuner/vocal analysis.
 * Algorithm (de Cheveigné & Kawahara, JASA 2002):
 *   1. Difference function: d(τ) = Σ(x[j] - x[j+τ])²
 *   2. Cumulative mean normalized difference: d'(τ) = d(τ) / mean(d[1..τ])
 *   3. Find first τ where d'(τ) < threshold
 *   4. Parabolic interpolation for sub-sample accuracy
 *   5. pitch = sample_rate / best_tau
 *
 * SSE2: vectorize the inner loop of the difference function.
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <emmintrin.h>
#include "wbus.h"

#define YIN_DEFAULT_THRESHOLD 0.15f
#define YIN_MIN_TAU 20       /* ~2200 Hz at 44.1k */
#define YIN_MAX_TAU 800      /* ~55 Hz at 44.1k */

typedef struct {
    uint32_t sr;
    float    threshold;
    int      min_tau;
    int      max_tau;
} wb_yin_cfg;

/* SSE2 YIN difference function for one tau: process 4 samples at once */
static float yin_diff_sse2(const float *buf, int n, int tau, int buf_len) {
    int len = buf_len - tau;
    if (len > n) len = n;
    int i = 0;
    __m128 sum4 = _mm_setzero_ps();

    for (; i + 3 < len; i += 4) {
        __m128 a = _mm_loadu_ps(&buf[i]);
        __m128 b = _mm_loadu_ps(&buf[i + tau]);
        __m128 diff = _mm_sub_ps(a, b);
        sum4 = _mm_add_ps(sum4, _mm_mul_ps(diff, diff));
    }

    /* Horizontal sum */
    float result[4];
    _mm_storeu_ps(result, sum4);
    float sum = result[0] + result[1] + result[2] + result[3];

    /* Tail */
    for (; i < len; i++) {
        float diff = buf[i] - buf[i + tau];
        sum += diff * diff;
    }
    return sum;
}

/* Run YIN pitch detection on a buffer of audio.
 * Returns pitch in Hz, or 0 if no pitch detected.
 * buf: audio samples (mono, float)
 * n: number of samples
 * cfg: YIN configuration (threshold, min/max tau, sample rate) */
float wb_yin_detect(const float *buf, int n, const wb_yin_cfg *cfg) {
    if (!buf || n < 64 || !cfg) return 0.0f;

    int min_tau = cfg->min_tau;
    int max_tau = cfg->max_tau;
    if (max_tau > n / 2) max_tau = n / 2;
    if (min_tau < 1) min_tau = 1;
    if (min_tau >= max_tau) return 0.0f;

    float threshold = cfg->threshold;
    if (threshold <= 0.0f) threshold = YIN_DEFAULT_THRESHOLD;

    /* Compute difference function */
    float *d = (float *)calloc(max_tau + 1, sizeof(float));
    if (!d) return 0.0f;

    for (int tau = 0; tau <= max_tau; tau++) {
        d[tau] = yin_diff_sse2(buf, n, tau, n);
    }

    /* Cumulative mean normalized difference */
    float *dn = (float *)calloc(max_tau + 1, sizeof(float));
    if (!dn) { free(d); return 0.0f; }

    dn[0] = 1.0f;
    float running_sum = 0.0f;
    for (int tau = 1; tau <= max_tau; tau++) {
        running_sum += d[tau];
        dn[tau] = d[tau] / (running_sum / (float)tau);
    }

    /* Find first minimum below threshold */
    int best_tau = 0;
    for (int tau = min_tau; tau < max_tau; tau++) {
        if (dn[tau] < threshold) {
            /* Find the local minimum */
            while (tau + 1 < max_tau && dn[tau + 1] < dn[tau]) {
                tau++;
            }
            best_tau = tau;
            break;
        }
    }

    /* If no dip below threshold, find global minimum in range */
    if (best_tau == 0) {
        float min_val = dn[min_tau];
        for (int tau = min_tau + 1; tau < max_tau; tau++) {
            if (dn[tau] < min_val) {
                min_val = dn[tau];
                best_tau = tau;
            }
        }
    }

    /* Parabolic interpolation for sub-sample accuracy */
    float pitch = 0.0f;
    if (best_tau > 0 && best_tau < max_tau) {
        float s0 = dn[best_tau - 1];
        float s1 = dn[best_tau];
        float s2 = dn[best_tau + 1];
        float denom = 2.0f * (2.0f * s1 - s0 - s2);
        if (fabsf(denom) > 1e-9f) {
            float delta = (s0 - s2) / denom;
            float refined_tau = (float)best_tau + delta;
            if (refined_tau > 0) {
                pitch = (float)cfg->sr / refined_tau;
            }
        } else {
            pitch = (float)cfg->sr / (float)best_tau;
        }
    }

    free(d);
    free(dn);
    return pitch;
}

/* Convenience: detect pitch with default config */
float wb_yin_pitch(const float *buf, int n, uint32_t sr) {
    wb_yin_cfg cfg = {
        .sr = sr,
        .threshold = YIN_DEFAULT_THRESHOLD,
        .min_tau = YIN_MIN_TAU,
        .max_tau = YIN_MAX_TAU
    };
    /* Scale max_tau for sample rate */
    cfg.max_tau = (int)((float)cfg.max_tau * 44100.0f / (float)sr);
    cfg.min_tau = (int)((float)cfg.min_tau * 44100.0f / (float)sr);
    if (cfg.min_tau < 2) cfg.min_tau = 2;
    return wb_yin_detect(buf, n, &cfg);
}
