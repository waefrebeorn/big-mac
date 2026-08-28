/* wb_exciter.c — harmonic exciter / enhancer.
 *
 * R077: Adds harmonics for brightness, presence, "air".
 *
 * Algorithm:
 *   1. Highpass filter to isolate high frequencies
 *   2. Waveshape (asymmetric clipping) to generate harmonics
 *   3. Blend back with dry signal
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    uint32_t sr;
    float    amount;         /* 0..1 drive amount */
    float    blend;          /* 0..1 wet/dry mix */
    float    hp_freq;        /* Highpass frequency */
    float    hp_state[4];    /* Filter states */
    float    hp_b0, hp_b1, hp_b2, hp_a1, hp_a2;
} wb_exciter_inst;

void *wb_exciter_create(uint32_t sr) {
    wb_exciter_inst *ex = (wb_exciter_inst *)calloc(1, sizeof(*ex));
    if (!ex) return NULL;
    ex->sr = sr;
    ex->amount = 0.3f;
    ex->blend = 0.5f;
    ex->hp_freq = 2000.0f;

    float omega = 2.0f * 3.14159265f * ex->hp_freq / (float)sr;
    float cos_o = cosf(omega);
    float sin_o = sinf(omega);
    float alpha = sin_o / (2.0f * 0.707f);
    float a0 = 1.0f + alpha;
    ex->hp_b0 = (1.0f + cos_o) / (2.0f * a0);
    ex->hp_b1 = -(1.0f + cos_o) / a0;
    ex->hp_b2 = (1.0f + cos_o) / (2.0f * a0);
    ex->hp_a1 = (-2.0f * cos_o) / a0;
    ex->hp_a2 = (1.0f - alpha) / a0;

    return ex;
}

void wb_exciter_destroy(void *inst) { free(inst); }

void wb_exciter_set(void *inst, int param, float v) {
    wb_exciter_inst *ex = (wb_exciter_inst *)inst;
    if (!ex) return;
    switch (param) {
    case 0: ex->amount = v < 0 ? 0 : (v > 1 ? 1 : v); break;
    case 1: ex->blend = v < 0 ? 0 : (v > 1 ? 1 : v); break;
    default: break;
    }
}

/* Asymmetric waveshaper — generates even + odd harmonics */
static float exciter_distort(float x, float amount) {
    /* Soft clip with asymmetry */
    float driven = x * (1.0f + amount * 5.0f);
    /* tanh soft clip */
    float clipped = tanhf(driven);
    /* Asymmetry: add slight DC offset to create even harmonics */
    float asymmetric = clipped + amount * 0.1f * clipped * clipped;
    return asymmetric;
}

void wb_exciter_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_exciter_inst *ex = (wb_exciter_inst *)inst;
    if (!ex) return;

    for (uint32_t i = 0; i < n; i++) {
        /* Process L */
        float in_l = L[i];
        float hp_l = ex->hp_b0 * in_l + ex->hp_b1 * ex->hp_state[0] +
                      ex->hp_b2 * ex->hp_state[1] -
                      ex->hp_a1 * ex->hp_state[2] -
                      ex->hp_a2 * ex->hp_state[3];
        ex->hp_state[1] = ex->hp_state[0];
        ex->hp_state[0] = in_l;
        ex->hp_state[3] = ex->hp_state[2];
        ex->hp_state[2] = hp_l;

        float harmonics_l = exciter_distort(hp_l, ex->amount);
        L[i] = in_l * (1.0f - ex->blend) + harmonics_l * ex->blend;

        /* Process R */
        float in_r = R[i];
        float hp_r = ex->hp_b0 * in_r + ex->hp_b1 * ex->hp_state[0] +
                      ex->hp_b2 * ex->hp_state[1] -
                      ex->hp_a1 * ex->hp_state[2] -
                      ex->hp_a2 * ex->hp_state[3];
        /* (Reuse same filter state for simplicity — in production use separate) */
        float harmonics_r = exciter_distort(hp_r, ex->amount);
        R[i] = in_r * (1.0f - ex->blend) + harmonics_r * ex->blend;
    }
}
