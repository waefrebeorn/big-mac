/* wb_fuzz.c — Electro-Harmonix Big Muff Pi fuzz emulation.
 *
 * VST recreation: iconic fuzz pedal with cascaded clipping stages
 * and tone stack.
 *
 * Algorithm:
 *   3 cascaded clipping stages (transistor-like soft clip)
 *   + tone stack (Big Muff-style mid-scoop)
 *   + sustain/drive control
 *
 *   Clipping: y = x / (1 + |x|^n)^(1/n) — smooth soft clipper
 *   Tone stack: lowpass + highpass with mid cut
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    uint32_t sr;
    float    drive;      /* 0..1 → input gain 1x..50x */
    float    tone;       /* 0..1 → scoop to bright */
    float    level;      /* output volume */
    float    mix;        /* wet/dry */
    /* Tone stack states (biquad) */
    float    lp_state[4]; /* 2-stage lowpass */
    float    hp_state[4]; /* 2-stage highpass */
    float    lp_b0, lp_b1, lp_b2, lp_a1, lp_a2;
    float    hp_b0, hp_b1, hp_b2, hp_a1, hp_a2;
} wb_fuzz_inst;

void *wb_fuzz_create(uint32_t sr) {
    wb_fuzz_inst *f = (wb_fuzz_inst *)calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->sr = sr;
    f->drive = 0.5f;
    f->tone = 0.5f;
    f->level = 0.7f;
    f->mix = 1.0f;
    /* Default tone stack: mid-scoop */
    f->lp_b0 = 0.001f; f->lp_b1 = 0.002f; f->lp_b2 = 0.001f;
    f->lp_a1 = -1.96f; f->lp_a2 = 0.96f;
    f->hp_b0 = 0.98f; f->hp_b1 = -1.96f; f->hp_b2 = 0.98f;
    f->hp_a1 = -1.96f; f->hp_a2 = 0.96f;
    return f;
}

void wb_fuzz_destroy(void *inst) {
    free(inst);
}

void wb_fuzz_set(void *inst, int param, float v) {
    wb_fuzz_inst *f = (wb_fuzz_inst *)inst;
    if (!f) return;
    switch (param) {
    case 0: /* drive/sustain */
        f->drive = v < 0 ? 0 : (v > 1 ? 1 : v);
        break;
    case 1: /* tone */
        f->tone = v < 0 ? 0 : (v > 1 ? 1 : v);
        break;
    case 2: /* level */
        f->level = v < 0 ? 0 : (v > 2 ? 2 : v);
        break;
    case 3: /* mix */
        f->mix = v < 0 ? 0 : (v > 1 ? 1 : v);
        break;
    default: break;
    }
}

/* Soft clipper: smooth transistor-like distortion */
static inline float soft_clip(float x) {
    /* x / sqrt(1 + x^2) — smooth saturation */
    return x / sqrtf(1.0f + x * x);
}

/* Harder clipper for later stages */
static inline float hard_clip(float x) {
    if (x > 1.0f) return 1.0f;
    if (x < -1.0f) return -1.0f;
    return x;
}

/* Biquad process (transposed Form II) */
static inline float biquad(float x, float b0, float b1, float b2,
                            float a1, float a2, float *s1, float *s2) {
    float y = b0 * x + *s1;
    *s1 = b1 * x - a1 * y + *s2;
    *s2 = b2 * x - a2 * y;
    return y;
}

/* Big Muff tone stack: scooped mids */
static float tone_stack(wb_fuzz_inst *f, float x) {
    /* Low pass for bass */
    float lp = biquad(x, f->lp_b0, f->lp_b1, f->lp_b2, f->lp_a1, f->lp_a2,
                      &f->lp_state[0], &f->lp_state[1]);
    lp = biquad(lp, f->lp_b0, f->lp_b1, f->lp_b2, f->lp_a1, f->lp_a2,
                &f->lp_state[2], &f->lp_state[3]);
    /* High pass for treble */
    float hp = biquad(x, f->hp_b0, f->hp_b1, f->hp_b2, f->hp_a1, f->hp_a2,
                      &f->hp_state[0], &f->hp_state[1]);
    hp = biquad(hp, f->hp_b0, f->hp_b1, f->hp_b2, f->hp_a1, f->hp_a2,
                &f->hp_state[2], &f->hp_state[3]);
    /* Mix: bass + treble (scoop mids) */
    float bass_amt = (1.0f - f->tone) * 0.5f;
    float treble_amt = f->tone * 0.5f;
    return lp * bass_amt + hp * treble_amt + x * 0.5f;
}

static inline float fuzz_process(wb_fuzz_inst *f, float x) {
    /* Apply drive */
    float gain = 1.0f + f->drive * 49.0f;  /* 1x to 50x */
    float s = x * gain;

    /* 3 cascaded clipping stages */
    s = soft_clip(s);
    s = soft_clip(s * 0.8f);
    s = hard_clip(s * 0.7f);

    /* Tone stack */
    s = tone_stack(f, s);

    /* Level + clamp */
    s *= f->level;
    if (s > 5.0f) s = 5.0f;
    if (s < -5.0f) s = -5.0f;

    /* Mix */
    return x * (1.0f - f->mix) + s * f->mix;
}

void wb_fuzz_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_fuzz_inst *f = (wb_fuzz_inst *)inst;
    if (!f) return;

    for (uint32_t i = 0; i < n; i++) {
        L[i] = fuzz_process(f, L[i]);
        R[i] = fuzz_process(f, R[i]);
    }
}
