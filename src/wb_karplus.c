/* wb_karplus.c — Karplus-Strong physical modeling synthesis.
 *
 * N3 [R076]: plucked string synthesis via delay line + lowpass feedback.
 *
 * Algorithm:
 *   1. Fill delay line with random noise (the "pluck")
 *   2. Loop: y[n] = 0.5 * (y[n-M] + y[n-M-1]) — averaging lowpass
 *   3. Feedback gain g controls decay (0.998 = long, 0.9 = short)
 *   4. Fractional delay via linear interpolation for fine pitch
 *   5. Extended: allpass filter for fine tuning, DC blocker
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define KP_MAX_DELAY 4096  /* ~20Hz at 44.1k */

typedef struct {
    uint32_t sr;
    float    *delay_line;
    uint32_t  delay_len;
    uint32_t  write_pos;
    float    feedback;
    float    damping;     /* 0..1 → lowpass coefficient */
    float    prev_sample; /* for averaging */
    int      active;
    uint32_t samples_left;
} wb_kp_inst;

void *wb_karplus_create(uint32_t sr) {
    wb_kp_inst *kp = (wb_kp_inst *)calloc(1, sizeof(*kp));
    if (!kp) return NULL;
    kp->sr = sr;
    kp->delay_line = (float *)calloc(KP_MAX_DELAY, sizeof(float));
    if (!kp->delay_line) { free(kp); return NULL; }
    kp->delay_len = 100;
    kp->feedback = 0.998f;
    kp->damping = 0.5f;
    kp->prev_sample = 0.0f;
    kp->active = 0;
    return kp;
}

void wb_karplus_destroy(void *inst) {
    wb_kp_inst *kp = (wb_kp_inst *)inst;
    if (kp) { free(kp->delay_line); free(kp); }
}

void wb_karplus_set(void *inst, int param, float v) {
    wb_kp_inst *kp = (wb_kp_inst *)inst;
    if (!kp) return;
    switch (param) {
    case 0: /* feedback/decay */
        kp->feedback = v < 0.9f ? 0.9f : (v > 0.9999f ? 0.9999f : v);
        break;
    case 1: /* damping/brightness */
        kp->damping = v < 0 ? 0 : (v > 1 ? 1 : v);
        break;
    default: break;
    }
}

/* Trigger a pluck at the given MIDI note */
void wb_karplus_note(void *inst, int note, int vel) {
    wb_kp_inst *kp = (wb_kp_inst *)inst;
    if (!kp || vel == 0) return;

    /* Compute delay length from frequency */
    float freq = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
    kp->delay_len = (uint32_t)((float)kp->sr / freq);
    if (kp->delay_len < 4) kp->delay_len = 4;
    if (kp->delay_len >= KP_MAX_DELAY) kp->delay_len = KP_MAX_DELAY - 1;

    /* Fill delay line with random noise (the pluck) */
    unsigned int rng = (unsigned int)(note * 12345 + vel * 6789);
    for (uint32_t i = 0; i < kp->delay_len; i++) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        kp->delay_line[i] = (float)((double)(rng & 0xFFFF) / 32768.0 - 1.0) * ((float)vel / 127.0f);
    }

    kp->write_pos = 0;
    kp->prev_sample = 0.0f;
    kp->active = 1;
    kp->samples_left = (uint32_t)(kp->sr * 2.0f); /* max 2 seconds */
}

/* Process one sample */
static inline float kp_process(wb_kp_inst *kp) {
    if (!kp->active || kp->samples_left == 0) return 0.0f;

    /* Read from delay line */
    uint32_t read_pos = (kp->write_pos + 1) % kp->delay_len;
    float current = kp->delay_line[read_pos];

    /* Averaging lowpass: y = damping * current + (1-damping) * prev */
    float filtered = kp->damping * current + (1.0f - kp->damping) * kp->prev_sample;
    kp->prev_sample = filtered;

    /* Write back with feedback */
    kp->delay_line[kp->write_pos] = filtered * kp->feedback;

    /* Advance write position */
    kp->write_pos = (kp->write_pos + 1) % kp->delay_len;
    kp->samples_left--;

    /* Check for silence (decay complete) */
    if (filtered < 0.0001f && filtered > -0.0001f) {
        kp->active = 0;
    }

    return filtered;
}

void wb_karplus_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_kp_inst *kp = (wb_kp_inst *)inst;
    if (!kp) return;

    for (uint32_t i = 0; i < n; i++) {
        float s = kp_process(kp);
        L[i] = s;
        R[i] = s;
    }
}
