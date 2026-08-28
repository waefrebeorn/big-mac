/* wb_transient_shaper.c — transient shaping (attack/sustain enhancement).
 *
 * R077 H8: SPL Differential Envelope method.
 *
 * Algorithm:
 *   1. Compute two envelope followers: fast (attack) and slow (sustain)
 *   2. Transient gain = fast_env / (slow_env + epsilon)
 *   3. Apply gain^shape to original signal
 *   4. Boost = enhance attacks, reduce = enhance sustain
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    uint32_t sr;
    float    boost;          /* -1..1 (negative = sustain enhancement) */
    float    attack_ms;      /* Fast envelope attack */
    float    release_ms;     /* Slow envelope release */
    float    fast_env;       /* Fast envelope state */
    float    slow_env;       /* Slow envelope state */
    float    fast_coeff;
    float    slow_coeff;
} wb_transient_shaper_inst;

void *wb_transient_shaper_create(uint32_t sr) {
    wb_transient_shaper_inst *ts = (wb_transient_shaper_inst *)calloc(1, sizeof(*ts));
    if (!ts) return NULL;
    ts->sr = sr;
    ts->boost = 0.5f;
    ts->attack_ms = 0.5f;
    ts->release_ms = 50.0f;
    ts->fast_env = 0;
    ts->slow_env = 0;
    ts->fast_coeff = expf(-1.0f / (ts->attack_ms * 0.001f * sr));
    ts->slow_coeff = expf(-1.0f / (ts->release_ms * 0.001f * sr));
    return ts;
}

void wb_transient_shaper_destroy(void *inst) { free(inst); }

void wb_transient_shaper_set(void *inst, int param, float v) {
    wb_transient_shaper_inst *ts = (wb_transient_shaper_inst *)inst;
    if (!ts) return;
    switch (param) {
    case 0: ts->boost = v < -1 ? -1 : (v > 1 ? 1 : v); break;
    case 1: ts->attack_ms = v > 0.01f ? v : 0.01f;
            ts->fast_coeff = expf(-1.0f / (ts->attack_ms * 0.001f * ts->sr)); break;
    case 2: ts->release_ms = v > 1.0f ? v : 1.0f;
            ts->slow_coeff = expf(-1.0f / (ts->release_ms * 0.001f * ts->sr)); break;
    default: break;
    }
}

/* Process stereo block. */
void wb_transient_shaper_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_transient_shaper_inst *ts = (wb_transient_shaper_inst *)inst;
    if (!ts) return;

    for (uint32_t i = 0; i < n; i++) {
        /* Input level */
        float input = fabsf(L[i]) > fabsf(R[i]) ? fabsf(L[i]) : fabsf(R[i]);

        /* Fast envelope (tracks attacks) */
        if (input > ts->fast_env) {
            ts->fast_env = ts->fast_coeff * ts->fast_env + (1.0f - ts->fast_coeff) * input;
        } else {
            ts->fast_env = 0.99f * ts->fast_env;  /* Fast decay */
        }

        /* Slow envelope (tracks sustain) */
        if (input > ts->slow_env) {
            ts->slow_env = ts->slow_coeff * ts->slow_env + (1.0f - ts->slow_coeff) * input;
        } else {
            ts->slow_env = ts->slow_coeff * ts->slow_env + (1.0f - ts->slow_coeff) * input;
        }

        /* Transient gain = fast / slow */
        float transient_gain = ts->fast_env / (ts->slow_env + 1e-10f);

        /* Map to gain factor */
        float gain;
        if (ts->boost > 0) {
            /* Enhance attacks: gain > 1 when transient is strong */
            gain = 1.0f + ts->boost * (transient_gain - 1.0f);
        } else {
            /* Enhance sustain: gain < 1 when transient is strong */
            gain = 1.0f + ts->boost * (1.0f - transient_gain);
        }

        /* Clamp */
        if (gain < 0.1f) gain = 0.1f;
        if (gain > 4.0f) gain = 4.0f;

        L[i] *= gain;
        R[i] *= gain;
    }
}
