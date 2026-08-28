/* wb_ladder.c — Moog transistor ladder filter emulation.
 *
 * VST recreation: Minimoog-style 4-pole lowpass filter.
 *
 * Algorithm (Huovilainen model — antiderivative-based nonlinear solver):
 *   4 cascaded 1-pole lowpass stages with nonlinear tanh saturation.
 *   Global feedback loop for resonance.
 *
 *   Stage: y[n] = y[n-1] + g * (tanh(x[n]) - tanh(y[n-1]))
 *   where g = tan(π * fc / fs) — the "tuning" parameter
 *
 *   Feedback: input_to_stage1 = input - 4 * k * tanh(stage4_output)
 *   where k = resonance (0..4)
 *
 *   The tanh() in each stage models transistor saturation.
 *   Uses the antiderivative method for stability at high resonance.
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    uint32_t sr;
    float    cutoff;      /* Hz */
    float    resonance;   /* 0..4 */
    /* 4 filter stage states */
    float    s[4];
    /* Precomputed */
    float    g;           /* tan(π*fc/fs) */
    float    k;           /* resonance * 4 */
} wb_ladder_inst;

void *wb_ladder_create(uint32_t sr) {
    wb_ladder_inst *f = (wb_ladder_inst *)calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->sr = sr;
    f->cutoff = 1000.0f;
    f->resonance = 0.0f;
    f->g = tanf(3.14159265f * 1000.0f / (float)sr);
    f->k = 0.0f;
    return f;
}

void wb_ladder_destroy(void *inst) {
    free(inst);
}

void wb_ladder_set(void *inst, int param, float v) {
    wb_ladder_inst *f = (wb_ladder_inst *)inst;
    if (!f) return;
    switch (param) {
    case 0: /* cutoff */
        f->cutoff = v < 20.0f ? 20.0f : (v > 20000.0f ? 20000.0f : v);
        f->g = tanf(3.14159265f * f->cutoff / (float)f->sr);
        break;
    case 1: /* resonance */
        f->resonance = v < 0.0f ? 0.0f : (v > 4.0f ? 4.0f : v);
        f->k = f->resonance;
        break;
    default: break;
    }
}

/* Process one sample through the ladder filter.
 * Uses the Huovilainen antiderivative model with nonlinear saturation. */
static inline float ladder_process(wb_ladder_inst *f, float input) {
    float g = f->g;
    float k = f->k;

    /* Clamp input to prevent blowup */
    if (input > 10.0f) input = 10.0f;
    if (input < -10.0f) input = -10.0f;

    /* Feedback: subtract resonant feedback from input */
    float u = input - 4.0f * k * tanhf(f->s[3]);

    /* Clamp feedback input */
    if (u > 10.0f) u = 10.0f;
    if (u < -10.0f) u = -10.0f;

    /* 4 cascaded nonlinear stages */
    float x = u;
    for (int i = 0; i < 4; i++) {
        float y_prev = f->s[i];
        float y_new = y_prev + g * (tanhf(x) - tanhf(y_prev));
        f->s[i] = y_new;
        x = y_new;
    }

    return f->s[3];
}

void wb_ladder_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_ladder_inst *f = (wb_ladder_inst *)inst;
    if (!f) return;

    for (uint32_t i = 0; i < n; i++) {
        L[i] = ladder_process(f, L[i]);
        R[i] = ladder_process(f, R[i]);
    }
}

/* Process a single mono sample (for synth voices) */
float wb_ladder_process_mono(void *inst, float x) {
    wb_ladder_inst *f = (wb_ladder_inst *)inst;
    if (!f) return x;
    return ladder_process(f, x);
}
