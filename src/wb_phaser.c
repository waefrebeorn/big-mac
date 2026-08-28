/* wb_phaser.c — phaser effect (allpass filter cascade with LFO).
 *
 * N5 [R076]: modulation effect — cascade of first-order allpass filters
 * with LFO-modulated center frequency creates sweeping notch comb filter.
 *
 * Algorithm (Zölzer DAFX):
 *   H(z) = (a + z^-1) / (1 + a*z^-1)
 *   where a = (tan(π*fc/fs) - 1) / (tan(π*fc/fs) + 1)
 *   LFO sweeps fc between min_freq and max_freq.
 *   Output = dry + wet * feedback
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define PHASER_DEFAULT_STAGES 6
#define PHASER_MAX_STAGES 8

typedef struct {
    uint32_t sr;
    int      stages;       /* number of allpass stages (2-8) */
    float    min_freq;     /* LFO minimum frequency (Hz) */
    float    max_freq;     /* LFO maximum frequency (Hz) */
    float    lfo_rate;     /* LFO speed (Hz) */
    float    feedback;     /* feedback amount (0-0.95) */
    float    mix;          /* wet/dry mix (0=dry, 1=wet) */
    float    lfo_phase;    /* current LFO phase (0-2π) */

    /* Per-stage state */
    float    x_prev[PHASER_MAX_STAGES];  /* x[n-1] */
    float    y_prev[PHASER_MAX_STAGES];  /* y[n-1] */
} wb_phaser_inst;

void *wb_phaser_create(uint32_t sr) {
    wb_phaser_inst *p = (wb_phaser_inst *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->sr = sr;
    p->stages = PHASER_DEFAULT_STAGES;
    p->min_freq = 200.0f;
    p->max_freq = 2000.0f;
    p->lfo_rate = 0.5f;
    p->feedback = 0.5f;
    p->mix = 0.5f;
    return p;
}

void wb_phaser_destroy(void *inst) {
    free(inst);
}

void wb_phaser_set(void *inst, int param, float v) {
    wb_phaser_inst *p = (wb_phaser_inst *)inst;
    if (!p) return;
    switch (param) {
    case 0: /* rate */
        p->lfo_rate = v < 0.01f ? 0.01f : (v > 20.0f ? 20.0f : v);
        break;
    case 1: /* depth */ /* maps to freq range */
        p->max_freq = p->min_freq + v * 4000.0f;
        if (p->max_freq > 8000.0f) p->max_freq = 8000.0f;
        break;
    case 2: /* feedback */
        p->feedback = v < 0 ? 0 : (v > 0.95f ? 0.95f : v);
        break;
    case 3: /* mix */
        p->mix = v < 0 ? 0 : (v > 1 ? 1 : v);
        break;
    case 4: /* stages */
        p->stages = (int)v;
        if (p->stages < 2) p->stages = 2;
        if (p->stages > PHASER_MAX_STAGES) p->stages = PHASER_MAX_STAGES;
        break;
    default: break;
    }
}

/* Process one sample through the phaser.
 * Returns the processed sample. */
static inline float phaser_process_sample(wb_phaser_inst *p, float x) {
    /* Compute LFO-modulated center frequency */
    float lfo = 0.5f + 0.5f * sinf(p->lfo_phase);
    float fc = p->min_freq + lfo * (p->max_freq - p->min_freq);

    /* Compute allpass coefficient */
    float tan_val = tanf(3.14159265f * fc / (float)p->sr);
    float a = (tan_val - 1.0f) / (tan_val + 1.0f);

    /* Process through allpass cascade with feedback */
    float input = x + p->feedback * p->y_prev[p->stages - 1];
    float y = input;

    for (int s = 0; s < p->stages; s++) {
        /* Allpass: y[n] = a*(x[n] - y[n-1]) + x[n-1] */
        float x_prev_s = p->x_prev[s];
        float y_prev_s = p->y_prev[s];
        float out = a * (y - y_prev_s) + x_prev_s;
        p->x_prev[s] = y;
        p->y_prev[s] = out;
        y = out;
    }

    /* Advance LFO phase */
    p->lfo_phase += 2.0f * 3.14159265f * p->lfo_rate / (float)p->sr;
    if (p->lfo_phase > 2.0f * 3.14159265f) p->lfo_phase -= 2.0f * 3.14159265f;

    /* Mix dry + wet */
    return x * (1.0f - p->mix) + y * p->mix;
}

void wb_phaser_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_phaser_inst *p = (wb_phaser_inst *)inst;
    if (!p) return;

    for (uint32_t i = 0; i < n; i++) {
        L[i] = phaser_process_sample(p, L[i]);
        R[i] = phaser_process_sample(p, R[i]);
    }
}
