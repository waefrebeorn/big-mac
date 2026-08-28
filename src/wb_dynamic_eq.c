/* wb_dynamic_eq.c — dynamic EQ (frequency-dependent compression).
 *
 * R077: Apply EQ that responds to signal level at each frequency.
 *
 * Algorithm:
 *   Split into bands via crossover filters
 *   Per-band: detect level, compute gain reduction, apply EQ
 *   Bands are independent — only compress frequencies that exceed threshold
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define DYNEQ_MAX_BANDS 4

typedef struct {
    float b0, b1, b2, a1, a2;  /* Biquad coefficients */
    float s1, s2;              /* State */
} biquad_t;

typedef struct {
    uint32_t sr;
    int      num_bands;
    float    crossover_freq[DYNEQ_MAX_BANDS - 1];

    /* Per-band parameters */
    float    threshold_db[DYNEQ_MAX_BANDS];
    float    ratio[DYNEQ_MAX_BANDS];
    float    attack_ms[DYNEQ_MAX_BANDS];
    float    release_ms[DYNEQ_MAX_BANDS];
    float    gain_db[DYNEQ_MAX_BANDS];      /* Static EQ gain */
    float    dynamic_gain_db[DYNEQ_MAX_BANDS]; /* Dynamic EQ gain */

    /* Filter bank */
    biquad_t lp[DYNEQ_MAX_BANDS - 1];
    biquad_t hp[DYNEQ_MAX_BANDS - 1];

    /* Per-band state */
    float    env_db[DYNEQ_MAX_BANDS];
    float    attack_coeff[DYNEQ_MAX_BANDS];
    float    release_coeff[DYNEQ_MAX_BANDS];
    float    gain_reduction[DYNEQ_MAX_BANDS];
} wb_dynamic_eq_inst;

static void biquad_init(biquad_t *f, float b0, float b1, float b2, float a1, float a2) {
    f->b0 = b0; f->b1 = b1; f->b2 = b2; f->a1 = a1; f->a2 = a2;
    f->s1 = f->s2 = 0;
}

static float biquad_process(biquad_t *f, float x) {
    float y = f->b0 * x + f->s1;
    f->s1 = f->b1 * x - f->a1 * y + f->s2;
    f->s2 = f->b2 * x - f->a2 * y;
    return y;
}

void *wb_dynamic_eq_create(uint32_t sr) {
    wb_dynamic_eq_inst *eq = (wb_dynamic_eq_inst *)calloc(1, sizeof(*eq));
    if (!eq) return NULL;
    eq->sr = sr;
    eq->num_bands = 3;
    eq->crossover_freq[0] = 200.0f;
    eq->crossover_freq[1] = 2000.0f;

    /* Default parameters per band */
    for (int i = 0; i < DYNEQ_MAX_BANDS; i++) {
        eq->threshold_db[i] = -24.0f;
        eq->ratio[i] = 3.0f;
        eq->attack_ms[i] = 5.0f;
        eq->release_ms[i] = 50.0f;
        eq->gain_db[i] = 0.0f;
        eq->dynamic_gain_db[i] = 0.0f;
        eq->env_db[i] = -100.0f;
        eq->attack_coeff[i] = expf(-1.0f / (eq->attack_ms[i] * 0.001f * sr));
        eq->release_coeff[i] = expf(-1.0f / (eq->release_ms[i] * 0.001f * sr));
        eq->gain_reduction[i] = 0.0f;
    }

    /* Initialize crossover filters (Linkwitz-Riley 2nd order) */
    for (int i = 0; i < eq->num_bands - 1; i++) {
        float fc = eq->crossover_freq[i];
        float omega = 2.0f * 3.14159265f * fc / (float)sr;
        float cos_o = cosf(omega);
        float sin_o = sinf(omega);
        float Q = 0.707f;
        float alpha = sin_o / (2.0f * Q);
        float a0 = 1.0f + alpha;

        float b0_lp = (1.0f - cos_o) / (2.0f * a0);
        float b1_lp = (1.0f - cos_o) / a0;
        float b2_lp = (1.0f - cos_o) / (2.0f * a0);
        float a1 = (-2.0f * cos_o) / a0;
        float a2 = (1.0f - alpha) / a0;

        biquad_init(&eq->lp[i], b0_lp, b1_lp, b2_lp, a1, a2);
        biquad_init(&eq->hp[i], (1.0f+cos_o)/(2.0f*a0), -(1.0f+cos_o)/a0, (1.0f+cos_o)/(2.0f*a0), a1, a2);
    }

    return eq;
}

void wb_dynamic_eq_destroy(void *inst) { free(inst); }

void wb_dynamic_eq_set(void *inst, int param, float v) {
    wb_dynamic_eq_inst *eq = (wb_dynamic_eq_inst *)inst;
    if (!eq) return;
    /* param = band * 10 + setting */
    int band = param / 10;
    int setting = param % 10;
    if (band >= DYNEQ_MAX_BANDS) return;

    switch (setting) {
    case 0: eq->threshold_db[band] = v; break;
    case 1: eq->ratio[band] = v > 1.0f ? v : 1.0f; break;
    case 2: eq->gain_db[band] = v; break;
    default: break;
    }
}

/* Process a stereo block. */
void wb_dynamic_eq_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_dynamic_eq_inst *eq = (wb_dynamic_eq_inst *)inst;
    if (!eq) return;

    for (uint32_t i = 0; i < n; i++) {
        float mono = (L[i] + R[i]) * 0.5f;

        /* Split into bands */
        float bands[DYNEQ_MAX_BANDS];
        float remaining = mono;

        for (int b = 0; b < eq->num_bands - 1; b++) {
            bands[b] = biquad_process(&eq->lp[b], remaining);
            float hp = biquad_process(&eq->hp[b], remaining);
            remaining = hp;
        }
        bands[eq->num_bands - 1] = remaining;  /* Highest band */

        /* Process each band */
        float output = 0;
        for (int b = 0; b < eq->num_bands; b++) {
            float input = bands[b];
            float input_db = 20.0f * log10f(fabsf(input) + 1e-10f);

            /* Envelope */
            if (input_db > eq->env_db[b]) {
                eq->env_db[b] = eq->attack_coeff[b] * eq->env_db[b] +
                                (1.0f - eq->attack_coeff[b]) * input_db;
            } else {
                eq->env_db[b] = eq->release_coeff[b] * eq->env_db[b] +
                                (1.0f - eq->release_coeff[b]) * input_db;
            }

            /* Gain reduction */
            float over_db = eq->env_db[b] - eq->threshold_db[b];
            float gr_db = (over_db > 0) ? (1.0f - 1.0f / eq->ratio[b]) * over_db : 0.0f;
            eq->gain_reduction[b] = gr_db;

            /* Total gain = static EQ + dynamic */
            float total_gain_db = eq->gain_db[b] - gr_db;
            float gain_linear = powf(10.0f, total_gain_db / 20.0f);

            output += input * gain_linear;
        }

        L[i] = output;
        R[i] = output;
    }
}
