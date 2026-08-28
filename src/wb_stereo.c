/* wb_stereo.c — mid-side stereo widening + Haas effect + M/S utility.
 *
 * N1 [R076]: spatial audio processing for the mixer bus.
 * Techniques (from 7-hop research convergence):
 *   - Mid-Side encode/decode: M=(L+R)/2, S=(L-R)/2
 *   - Stereo widening: scale S channel before decode
 *   - Frequency-dependent width: split into bands via Linkwitz-Riley
 *     crossover, apply independent width per band (mono bass, wide highs)
 *   - Haas effect: delay one channel 1-40ms for precedence-based width
 *
 * Pure C11, SSE2 intrinsics. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <emmintrin.h>
#include "wbus.h"

/* ---- SSE2 M/S widening: process 4 stereo samples at once ---- */
void wb_stereo_widen4(const wb_sample *in, wb_sample *out, uint32_t n,
                      float width) {
    __m128 half = _mm_set1_ps(0.5f);
    __m128 w_vec = _mm_set1_ps(width);
    uint32_t i = 0;

    /* Process 4 stereo samples (8 floats) per iteration */
    for (; i + 3 < n; i += 4) {
        /* Load 4 interleaved stereo pairs */
        __m128 a = _mm_loadu_ps(&in[i * 2]);      /* [L0, R0, L1, R1] */
        __m128 b = _mm_loadu_ps(&in[i * 2 + 4]);  /* [L2, R2, L3, R3] */

        /* Deinterleave into L = [L0,L1,L2,L3] and R = [R0,R1,R2,R3] */
        __m128 tmp0 = _mm_unpacklo_ps(a, b);  /* [L0, L2, R0, R2] */
        __m128 tmp1 = _mm_unpackhi_ps(a, b);  /* [L1, L3, R1, R3] */
        __m128 L_vec = _mm_unpacklo_ps(tmp0, tmp1);  /* [L0, L1, L2, L3] */
        __m128 R_vec = _mm_unpackhi_ps(tmp0, tmp1);  /* [R0, R1, R2, R3] */

        /* M/S encode */
        __m128 M_vec = _mm_mul_ps(_mm_add_ps(L_vec, R_vec), half);
        __m128 S_vec = _mm_mul_ps(_mm_sub_ps(L_vec, R_vec), half);

        /* Apply width to side channel */
        S_vec = _mm_mul_ps(S_vec, w_vec);

        /* M/S decode: L = M + S, R = M - S (no 0.5 — already in encode) */
        __m128 L_out = _mm_add_ps(M_vec, S_vec);
        __m128 R_out = _mm_sub_ps(M_vec, S_vec);

        /* Reinterleave back to [L0, R0, L1, R1, L2, R2, L3, R3] */
        __m128 out0 = _mm_unpacklo_ps(L_out, R_out); /* [L0, R0, L1, R1] */
        __m128 out1 = _mm_unpackhi_ps(L_out, R_out); /* [L2, R2, L3, R3] */

        _mm_storeu_ps(&out[i * 2], out0);
        _mm_storeu_ps(&out[i * 2 + 4], out1);
    }

    /* Tail: scalar */
    for (; i < n; i++) {
        float L = in[i * 2], R = in[i * 2 + 1];
        float M = (L + R) * 0.5f, S = (L - R) * 0.5f;
        S *= width;
        out[i * 2]     = M + S;
        out[i * 2 + 1] = M - S;
    }
}

/* Scalar M/S widening (for reference/testing) */
void wb_stereo_widen(const wb_sample *in, wb_sample *out, uint32_t n,
                     float width) {
    for (uint32_t i = 0; i < n; i++) {
        float L = in[i * 2], R = in[i * 2 + 1];
        float M = (L + R) * 0.5f, S = (L - R) * 0.5f;
        S *= width;
        out[i * 2]     = M + S;
        out[i * 2 + 1] = M - S;
    }
}

/* ---- Haas Effect Stereo Widener ---- */

typedef struct {
    wb_sample *delay_buf;
    uint32_t   buf_size;
    uint32_t   write_pos;
    uint32_t   delay_samples;
} wb_haas_inst;

void *wb_haas_create(uint32_t sr, float delay_ms) {
    wb_haas_inst *h = (wb_haas_inst *)calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->delay_samples = (uint32_t)(delay_ms * 0.001f * (float)sr);
    if (h->delay_samples < 1) h->delay_samples = 1;
    h->buf_size = h->delay_samples + 1;
    h->delay_buf = (wb_sample *)calloc(h->buf_size, sizeof(wb_sample));
    h->write_pos = 0;
    return h;
}

void wb_haas_destroy(void *inst) {
    wb_haas_inst *h = (wb_haas_inst *)inst;
    if (h) { free(h->delay_buf); free(h); }
}

void wb_haas_set_delay(void *inst, float delay_ms, uint32_t sr) {
    wb_haas_inst *h = (wb_haas_inst *)inst;
    if (!h) return;
    h->delay_samples = (uint32_t)(delay_ms * 0.001f * (float)sr);
    if (h->delay_samples < 1) h->delay_samples = 1;
    if (h->delay_samples >= h->buf_size) {
        free(h->delay_buf);
        h->buf_size = h->delay_samples + 1;
        h->delay_buf = (wb_sample *)calloc(h->buf_size, sizeof(wb_sample));
    }
    h->write_pos = 0;
}

/* Process: delay the right channel by delay_samples.
 * Left channel passes through dry. */
void wb_haas_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_haas_inst *h = (wb_haas_inst *)inst;
    (void)L; /* Haas only delays right channel */
    if (!h || !h->delay_buf) return;

    for (uint32_t i = 0; i < n; i++) {
        h->delay_buf[h->write_pos] = R[i];
        uint32_t read_pos = (h->write_pos + h->buf_size - h->delay_samples) % h->buf_size;
        R[i] = h->delay_buf[read_pos];
        h->write_pos = (h->write_pos + 1) % h->buf_size;
    }
}

/* ---- Frequency-dependent stereo widening ---- */

typedef struct {
    /* Biquad states for 2-stage LR4 crossover (L and R channels) */
    float s1_l[4], s2_l[4];  /* low: 2 stages × 2 channels */
    float s1_h[4], s2_h[4];  /* high: 2 stages × 2 channels */
    float b0_l[2], b1_l[2], b2_l[2], a1_l[2], a2_l[2];
    float b0_h[2], b1_h[2], b2_h[2], a1_h[2], a2_h[2];
} wb_lr4xover;

/* Compute LR4 crossover coefficients for given fc and sr. */
void wb_lr4xover_compute(wb_lr4xover *x, float fc, uint32_t sr) {
    float omega = 2.0f * 3.14159265f * fc / (float)sr;
    float sin_omega = sinf(omega);
    float cos_omega = cosf(omega);
    float Q = 0.70710678f;  /* Butterworth */
    float alpha = sin_omega / (2.0f * Q);
    float a0 = 1.0f + alpha;

    for (int stage = 0; stage < 2; stage++) {
        x->b0_l[stage] = (1.0f - cos_omega) / (2.0f * a0);
        x->b1_l[stage] = (1.0f - cos_omega) / a0;
        x->b2_l[stage] = (1.0f - cos_omega) / (2.0f * a0);
        x->a1_l[stage] = (-2.0f * cos_omega) / a0;
        x->a2_l[stage] = (1.0f - alpha) / a0;

        x->b0_h[stage] = (1.0f + cos_omega) / (2.0f * a0);
        x->b1_h[stage] = -(1.0f + cos_omega) / a0;
        x->b2_h[stage] = (1.0f + cos_omega) / (2.0f * a0);
        x->a1_h[stage] = (-2.0f * cos_omega) / a0;
        x->a2_h[stage] = (1.0f - alpha) / a0;
    }
}

static inline float biquad_process(float x, float b0, float b1, float b2,
                                    float a1, float a2, float *s1, float *s2) {
    float y = b0 * x + *s1;
    *s1 = b1 * x - a1 * y + *s2;
    *s2 = b2 * x - a2 * y;
    return y;
}

/* Frequency-dependent stereo widening.
 * in/out are interleaved stereo, n = number of stereo samples.
 * low_width: width for frequencies below fc (typically 0 = mono bass).
 * high_width: width for frequencies above fc (typically 1.5-2.5 = widened). */
void wb_stereo_widen_fd(const wb_sample *in, wb_sample *out, uint32_t n,
                        float fc, float low_width, float high_width,
                        wb_lr4xover *xover, uint32_t sr) {
    if (!xover) return;
    (void)fc;
    (void)sr;

    for (uint32_t i = 0; i < n; i++) {
        float L = in[i * 2];
        float R = in[i * 2 + 1];

        /* Split L into low/high bands */
        float L_low  = biquad_process(L, xover->b0_l[0], xover->b1_l[0],
                                       xover->b2_l[0], xover->a1_l[0],
                                       xover->a2_l[0], &xover->s1_l[0], &xover->s2_l[0]);
        L_low = biquad_process(L_low, xover->b0_l[1], xover->b1_l[1],
                               xover->b2_l[1], xover->a1_l[1],
                               xover->a2_l[1], &xover->s1_l[1], &xover->s2_l[1]);
        float L_high = biquad_process(L, xover->b0_h[0], xover->b1_h[0],
                                       xover->b2_h[0], xover->a1_h[0],
                                       xover->a2_h[0], &xover->s1_h[0], &xover->s2_h[0]);
        L_high = biquad_process(L_high, xover->b0_h[1], xover->b1_h[1],
                                xover->b2_h[1], xover->a1_h[1],
                                xover->a2_h[1], &xover->s1_h[1], &xover->s2_h[1]);

        /* Split R into low/high bands */
        float R_low  = biquad_process(R, xover->b0_l[0], xover->b1_l[0],
                                       xover->b2_l[0], xover->a1_l[0],
                                       xover->a2_l[0], &xover->s1_l[2], &xover->s2_l[2]);
        R_low = biquad_process(R_low, xover->b0_l[1], xover->b1_l[1],
                               xover->b2_l[1], xover->a1_l[1],
                               xover->a2_l[1], &xover->s1_l[3], &xover->s2_l[3]);
        float R_high = biquad_process(R, xover->b0_h[0], xover->b1_h[0],
                                       xover->b2_h[0], xover->a1_h[0],
                                       xover->a2_h[0], &xover->s1_h[2], &xover->s2_h[2]);
        R_high = biquad_process(R_high, xover->b0_h[1], xover->b1_h[1],
                                xover->b2_h[1], xover->a1_h[1],
                                xover->a2_h[1], &xover->s1_h[3], &xover->s2_h[3]);

        /* Apply M/S widening per band */
        float M_low = (L_low + R_low) * 0.5f;
        float S_low = (L_low - R_low) * 0.5f * low_width;
        float L_low_out = M_low + S_low;
        float R_low_out = M_low - S_low;

        float M_high = (L_high + R_high) * 0.5f;
        float S_high = (L_high - R_high) * 0.5f * high_width;
        float L_high_out = M_high + S_high;
        float R_high_out = M_high - S_high;

        /* Recombine bands */
        out[i * 2]     = L_low_out + L_high_out;
        out[i * 2 + 1] = R_low_out + R_high_out;
    }
}
