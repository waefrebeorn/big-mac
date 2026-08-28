/* wb_bass_boost.c — bass boost / sub-bass enhancer.
 *
 * Meme/YTP essential: that earth-shaking low-end.
 *
 * Algorithm:
 *   1. Lowpass filter to isolate sub-bass region (<150Hz)
 *   2. Harmonic bass generation (octave up) for small speakers
 *   3. Dynamic compression of bass band
 *   4. Mix back with dry signal
 *
 * Uses cascaded biquad lowpass + waveshaping for harmonics.
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    uint32_t sr;
    float    amount;      /* 0..1 → boost amount */
    float    frequency;   /* cutoff Hz (default 120) */
    /* 4th-order Linkwitz-Riley lowpass states */
    float    lp_s1[4], lp_s2[4];
    float    lp_b0[2], lp_b1[2], lp_b2[2], lp_a1[2], lp_a2[2];
    /* Highpass to remove DC */
    float    hp_s1, hp_s2;
    float    hp_b0, hp_b1, hp_b2, hp_a1, hp_a2;
} wb_bass_boost_inst;

static void compute_lp_coeffs(wb_bass_boost_inst *b) {
    float omega = 2.0f * 3.14159265f * b->frequency / (float)b->sr;
    float sin_o = sinf(omega);
    float cos_o = cosf(omega);
    float Q = 0.70710678f;
    float alpha = sin_o / (2.0f * Q);
    float a0 = 1.0f + alpha;

    for (int stage = 0; stage < 2; stage++) {
        b->lp_b0[stage] = (1.0f - cos_o) / (2.0f * a0);
        b->lp_b1[stage] = (1.0f - cos_o) / a0;
        b->lp_b2[stage] = (1.0f - cos_o) / (2.0f * a0);
        b->lp_a1[stage] = (-2.0f * cos_o) / a0;
        b->lp_a2[stage] = (1.0f - alpha) / a0;
    }
}

void *wb_bass_boost_create(uint32_t sr) {
    wb_bass_boost_inst *b = (wb_bass_boost_inst *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->sr = sr;
    b->amount = 0.5f;
    b->frequency = 120.0f;
    compute_lp_coeffs(b);
    /* DC-blocking highpass at 20Hz */
    b->hp_b0 = 0.999f; b->hp_b1 = -1.998f; b->hp_b2 = 0.999f;
    b->hp_a1 = -1.998f; b->hp_a2 = 0.998f;
    return b;
}

void wb_bass_boost_destroy(void *inst) { free(inst); }

void wb_bass_boost_set(void *inst, int param, float v) {
    wb_bass_boost_inst *b = (wb_bass_boost_inst *)inst;
    if (!b) return;
    if (param == 0) {
        b->amount = v < 0 ? 0 : (v > 1 ? 1 : v);
    } else if (param == 1) {
        b->frequency = v < 20 ? 20 : (v > 300 ? 300 : v);
        compute_lp_coeffs(b);
    }
}

static inline float biquad(float x, float b0, float b1, float b2,
                            float a1, float a2, float *s1, float *s2) {
    float y = b0 * x + *s1;
    *s1 = b1 * x - a1 * y + *s2;
    *s2 = b2 * x - a2 * y;
    return y;
}

void wb_bass_boost_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_bass_boost_inst *b = (wb_bass_boost_inst *)inst;
    if (!b || b->amount <= 0) return;

    for (uint32_t i = 0; i < n; i++) {
        float dryL = L[i], dryR = R[i];

        /* Extract bass from mono sum */
        float mono = (dryL + dryR) * 0.5f;
        float bass = mono;

        /* 4th-order LR lowpass */
        for (int stage = 0; stage < 2; stage++) {
            bass = biquad(bass, b->lp_b0[stage], b->lp_b1[stage],
                          b->lp_b2[stage], b->lp_a1[stage], b->lp_a2[stage],
                          &b->lp_s1[stage*2], &b->lp_s2[stage*2]);
        }

        /* DC block */
        bass = biquad(bass, b->hp_b0, b->hp_b1, b->hp_b2, b->hp_a1, b->hp_a2,
                      &b->hp_s1, &b->hp_s2);

        /* Generate harmonics (octave up via full-wave rectification) */
        float harmonics = fabsf(bass) - bass * 0.5f; /* asymmetric clip */

        /* Boost = bass + harmonics, scaled by amount */
        float boost = (bass + harmonics * 0.5f) * b->amount * 3.0f;

        /* Mix back */
        L[i] = dryL + boost;
        R[i] = dryR + boost;
    }
}
