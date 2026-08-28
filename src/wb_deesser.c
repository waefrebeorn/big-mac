/* wb_deesser.c — vocal de-esser (sibilance reduction).
 *
 * R077: Essential for podcast/voice polishing.
 *
 * Algorithm:
 *   1. Bandpass filter to isolate sibilance range (4-10kHz)
 *   2. Detect sibilance level
 *   3. When sibilance exceeds threshold, attenuate that frequency band
 *   4. Smooth gain reduction to avoid artifacts
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    uint32_t sr;
    float    threshold_db;    /* Sibilance threshold */
    float    freq_center;     /* Center frequency (Hz) */
    float    bandwidth;       /* Bandwidth in octaves */
    float    reduction_db;    /* Max reduction in dB */

    /* State */
    float    sib_level;       /* Current sibilance level */
    float    gain_reduction;  /* Current gain reduction */
    float    attack_coeff;
    float    release_coeff;

    /* Bandpass filter states */
    float    bp_state[4];
    float    bp_b0, bp_b1, bp_b2, bp_a1, bp_a2;
} wb_deesser_inst;

void *wb_deesser_create(uint32_t sr) {
    wb_deesser_inst *ds = (wb_deesser_inst *)calloc(1, sizeof(*ds));
    if (!ds) return NULL;
    ds->sr = sr;
    ds->threshold_db = -30.0f;
    ds->freq_center = 7000.0f;
    ds->bandwidth = 1.0f;
    ds->reduction_db = -12.0f;
    ds->sib_level = 0;
    ds->gain_reduction = 0;
    ds->attack_coeff = expf(-1.0f / (1.0f * 0.001f * sr));   /* 1ms attack */
    ds->release_coeff = expf(-1.0f / (50.0f * 0.001f * sr));  /* 50ms release */

    /* Compute bandpass coefficients */
    float omega = 2.0f * 3.14159265f * ds->freq_center / (float)sr;
    float cos_o = cosf(omega);
    float sin_o = sinf(omega);
    float bw = ds->bandwidth;
    float alpha = sin_o * sinhf(logf(2.0f) / 2.0f * bw * omega / sin_o);
    float a0 = 1.0f + alpha;

    ds->bp_b0 = alpha / a0;
    ds->bp_b1 = 0.0f;
    ds->bp_b2 = -alpha / a0;
    ds->bp_a1 = (-2.0f * cos_o) / a0;
    ds->bp_a2 = (1.0f - alpha) / a0;

    return ds;
}

void wb_deesser_destroy(void *inst) { free(inst); }

void wb_deesser_set(void *inst, int param, float v) {
    wb_deesser_inst *ds = (wb_deesser_inst *)inst;
    if (!ds) return;
    switch (param) {
    case 0: ds->threshold_db = v; break;
    case 1: ds->freq_center = v > 1000 ? v : 1000; break;
    case 2: ds->reduction_db = v < -30 ? -30 : (v > 0 ? 0 : v); break;
    default: break;
    }
}

/* Process stereo block. */
void wb_deesser_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_deesser_inst *ds = (wb_deesser_inst *)inst;
    if (!ds) return;

    float threshold_linear = powf(10.0f, ds->threshold_db / 20.0f);
    float reduction_linear = powf(10.0f, ds->reduction_db / 20.0f);

    for (uint32_t i = 0; i < n; i++) {
        /* Extract sibilance band from mono sum */
        float mono = (L[i] + R[i]) * 0.5f;

        /* Bandpass filter */
        float sib = ds->bp_b0 * mono + ds->bp_b1 * ds->bp_state[0] +
                     ds->bp_b2 * ds->bp_state[1] -
                     ds->bp_a1 * ds->bp_state[2] -
                     ds->bp_a2 * ds->bp_state[3];
        ds->bp_state[1] = ds->bp_state[0];
        ds->bp_state[0] = mono;
        ds->bp_state[3] = ds->bp_state[2];
        ds->bp_state[2] = sib;

        /* Sibilance level (smoothed) */
        float sib_abs = fabsf(sib);
        if (sib_abs > ds->sib_level) {
            ds->sib_level = ds->attack_coeff * ds->sib_level + (1.0f - ds->attack_coeff) * sib_abs;
        } else {
            ds->sib_level = ds->release_coeff * ds->sib_level + (1.0f - ds->release_coeff) * sib_abs;
        }

        /* Compute gain reduction */
        float target_gr = 1.0f;
        if (ds->sib_level > threshold_linear) {
            float over = ds->sib_level / threshold_linear;
            float gr_db = ds->reduction_db * log10f(over);
            target_gr = powf(10.0f, gr_db / 20.0f);
        }

        /* Smooth gain reduction */
        if (target_gr < ds->gain_reduction) {
            ds->gain_reduction = 0.9f * ds->gain_reduction + 0.1f * target_gr;
        } else {
            ds->gain_reduction = 0.99f * ds->gain_reduction + 0.01f * target_gr;
        }

        /* Apply */
        L[i] *= ds->gain_reduction;
        R[i] *= ds->gain_reduction;
    }
}
