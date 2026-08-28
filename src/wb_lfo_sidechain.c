/* wb_lfo_sidechain.c — LFO-driven sidechain ducking.
 *
 * R077: Rhythmic ducking without audio trigger — follows MIDI/tempo.
 *
 * Modes:
 *   0: Sine LFO (smooth pumping)
 *   1: Triangle LFO (linear pump)
 *   2: Square LFO (hard duck)
 *   3: Saw LFO (fast attack, slow release)
 *   4: Inverse saw (slow attack, fast release)
 *   5: MIDI-triggered envelope (duck on each note)
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    uint32_t sr;
    int      mode;
    float    rate_hz;        /* LFO rate (or beat division) */
    float    depth;          /* Duck depth (0..1, 1 = full duck) */
    float    phase;          /* Current LFO phase */
    float    phase_inc;      /* Phase increment per sample */

    /* MIDI-triggered envelope */
    float    env;            /* Current envelope value */
    float    attack_coeff;
    float    release_coeff;
    int      triggered;

    /* Tempo sync */
    float    bpm;
    int      beat_divisor;   /* 1=quarter, 2=eighth, 4=sixteenth */
} wb_lfo_sidechain_inst;

void *wb_lfo_sidechain_create(uint32_t sr) {
    wb_lfo_sidechain_inst *lc = (wb_lfo_sidechain_inst *)calloc(1, sizeof(*lc));
    if (!lc) return NULL;
    lc->sr = sr;
    lc->mode = 0;
    lc->rate_hz = 2.0f;  /* 2 Hz = 120 BPM eighth notes */
    lc->depth = 0.7f;
    lc->phase = 0.0f;
    lc->phase_inc = 2.0f * 3.14159265f * lc->rate_hz / (float)sr;
    lc->env = 1.0f;
    lc->attack_coeff = expf(-1.0f / (2.0f * 0.001f * sr));   /* 2ms attack */
    lc->release_coeff = expf(-1.0f / (150.0f * 0.001f * sr)); /* 150ms release */
    lc->triggered = 0;
    lc->bpm = 120.0f;
    lc->beat_divisor = 2;
    return lc;
}

void wb_lfo_sidechain_destroy(void *inst) { free(inst); }

void wb_lfo_sidechain_set(void *inst, int param, float v) {
    wb_lfo_sidechain_inst *lc = (wb_lfo_sidechain_inst *)inst;
    if (!lc) return;
    switch (param) {
    case 0: lc->mode = (int)v; break;
    case 1:
        lc->rate_hz = v > 0.01f ? v : 0.01f;
        lc->phase_inc = 2.0f * 3.14159265f * lc->rate_hz / (float)lc->sr;
        break;
    case 2: lc->depth = v < 0 ? 0 : (v > 1 ? 1 : v); break;
    case 3:
        lc->bpm = v > 20 ? v : 20;
        lc->rate_hz = (lc->bpm / 60.0f) * (float)lc->beat_divisor;
        lc->phase_inc = 2.0f * 3.14159265f * lc->rate_hz / (float)lc->sr;
        break;
    case 4: lc->beat_divisor = (int)v > 0 ? (int)v : 1; break;
    default: break;
    }
}

/* Trigger MIDI envelope (call on MIDI note-on) */
void wb_lfo_sidechain_trigger(void *inst) {
    wb_lfo_sidechain_inst *lc = (wb_lfo_sidechain_inst *)inst;
    if (!lc) return;
    lc->triggered = 1;
    lc->env = 0.0f;  /* Start at full duck */
}

/* Get current gain multiplier (1.0 = no duck, 0.0 = full duck) */
float wb_lfo_sidechain_get_gain(wb_lfo_sidechain_inst *lc) {
    if (!lc) return 1.0f;

    float lfo_value = 0.0f;

    switch (lc->mode) {
    case 0: /* Sine */
        lfo_value = 0.5f + 0.5f * sinf(lc->phase);
        break;
    case 1: /* Triangle */
        lfo_value = 1.0f - fabsf(lc->phase / 3.14159265f - 1.0f);
        break;
    case 2: /* Square */
        lfo_value = (lc->phase < 3.14159265f) ? 1.0f : 0.0f;
        break;
    case 3: /* Saw (fast attack, slow release) */
        lfo_value = 1.0f - lc->phase / (2.0f * 3.14159265f);
        break;
    case 4: /* Inverse saw */
        lfo_value = lc->phase / (2.0f * 3.14159265f);
        break;
    case 5: /* MIDI-triggered */
        if (lc->triggered) {
            /* Attack phase: quickly go to duck */
            if (lc->env < 0.01f) {
                lc->env += (1.0f - lc->env) * (1.0f - lc->attack_coeff);
            } else {
                /* Release phase: slowly recover */
                lc->env = lc->release_coeff * lc->env;
            }
            if (lc->env > 0.99f) {
                lc->env = 1.0f;
                lc->triggered = 0;
            }
            lfo_value = 1.0f - lc->env;
        } else {
            lfo_value = 1.0f;
        }
        break;
    default:
        lfo_value = 1.0f;
        break;
    }

    /* Advance LFO phase */
    if (lc->mode != 5) {
        lc->phase += lc->phase_inc;
        if (lc->phase > 2.0f * 3.14159265f) lc->phase -= 2.0f * 3.14159265f;
    }

    /* Map to gain: depth=1 means full duck at LFO=0 */
    return 1.0f - (1.0f - lfo_value) * lc->depth;
}

/* Process stereo block. */
void wb_lfo_sidechain_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_lfo_sidechain_inst *lc = (wb_lfo_sidechain_inst *)inst;
    if (!lc) return;

    for (uint32_t i = 0; i < n; i++) {
        float gain = wb_lfo_sidechain_get_gain(lc);
        L[i] *= gain;
        R[i] *= gain;
    }
}
