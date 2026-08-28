/* wb_parallel_comp.c — parallel compression (New York compression).
 *
 * R077: Blend compressed signal with dry for upward compression.
 *
 * Algorithm:
 *   1. Copy dry signal
 *   2. Heavily compress the copy
 *   3. Blend: output = dry * dry_gain + compressed * wet_gain
 *
 * This preserves transients while raising quiet details.
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    uint32_t sr;
    float    threshold_db;
    float    ratio;
    float    attack_ms;
    float    release_ms;
    float    dry_gain;      /* Dry signal gain (0..1) */
    float    wet_gain;      /* Compressed signal gain (0..2+) */
    float    makeup_db;

    /* Envelope state */
    float    env_db;
    float    attack_coeff;
    float    release_coeff;
} wb_parallel_comp_inst;

void *wb_parallel_comp_create(uint32_t sr) {
    wb_parallel_comp_inst *pc = (wb_parallel_comp_inst *)calloc(1, sizeof(*pc));
    if (!pc) return NULL;
    pc->sr = sr;
    pc->threshold_db = -24.0f;
    pc->ratio = 8.0f;
    pc->attack_ms = 2.0f;
    pc->release_ms = 100.0f;
    pc->dry_gain = 0.7f;
    pc->wet_gain = 0.5f;
    pc->makeup_db = 6.0f;
    pc->env_db = -100.0f;
    pc->attack_coeff = expf(-1.0f / (pc->attack_ms * 0.001f * sr));
    pc->release_coeff = expf(-1.0f / (pc->release_ms * 0.001f * sr));
    return pc;
}

void wb_parallel_comp_destroy(void *inst) { free(inst); }

void wb_parallel_comp_set(void *inst, int param, float v) {
    wb_parallel_comp_inst *pc = (wb_parallel_comp_inst *)inst;
    if (!pc) return;
    switch (param) {
    case 0: pc->threshold_db = v; break;
    case 1: pc->ratio = v > 1.0f ? v : 2.0f; break;
    case 2: pc->dry_gain = v < 0 ? 0 : (v > 1 ? 1 : v); break;
    case 3: pc->wet_gain = v < 0 ? 0 : (v > 2 ? 2 : v); break;
    case 4: pc->makeup_db = v; break;
    default: break;
    }
}

/* Process stereo block. */
void wb_parallel_comp_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_parallel_comp_inst *pc = (wb_parallel_comp_inst *)inst;
    if (!pc) return;

    float makeup_linear = powf(10.0f, pc->makeup_db / 20.0f);
    float slope = 1.0f - 1.0f / pc->ratio;

    for (uint32_t i = 0; i < n; i++) {
        /* Detector: max of L/R */
        float input = fabsf(L[i]) > fabsf(R[i]) ? fabsf(L[i]) : fabsf(R[i]);
        float input_db = 20.0f * log10f(input + 1e-10f);

        /* Envelope */
        if (input_db > pc->env_db) {
            pc->env_db = pc->attack_coeff * pc->env_db + (1.0f - pc->attack_coeff) * input_db;
        } else {
            pc->env_db = pc->release_coeff * pc->env_db + (1.0f - pc->release_coeff) * input_db;
        }

        /* Gain reduction */
        float over_db = pc->env_db - pc->threshold_db;
        float gr_db = (over_db > 0) ? slope * over_db : 0.0f;

        /* Compressed signal */
        float gain_linear = powf(10.0f, (-gr_db + pc->makeup_db) / 20.0f);

        /* Blend: dry + compressed */
        float dry_l = L[i] * pc->dry_gain;
        float dry_r = R[i] * pc->dry_gain;
        float wet_l = L[i] * gain_linear * pc->wet_gain;
        float wet_r = R[i] * gain_linear * pc->wet_gain;

        L[i] = dry_l + wet_l;
        R[i] = dry_r + wet_r;
    }
}

/* Quick preset: New York compression (heavy blend) */
void wb_parallel_comp_set_new_york(void *inst) {
    wb_parallel_comp_set(inst, 0, -30.0f);  /* threshold */
    wb_parallel_comp_set(inst, 1, 10.0f);   /* ratio */
    wb_parallel_comp_set(inst, 2, 0.6f);    /* dry */
    wb_parallel_comp_set(inst, 3, 0.8f);    /* wet */
    wb_parallel_comp_set(inst, 4, 8.0f);    /* makeup */
}