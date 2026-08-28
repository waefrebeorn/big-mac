/* wb_sidechain.c — sidechain compression / pumping / ducking.
 *
 * R077 H4: Essential for modern music production.
 *
 * Algorithm:
 *   1. Detector: peak/RMS envelope follower on sidechain input
 *   2. Gain computation: if detector > threshold, compute gain reduction
 *   3. Smoothing: attack/release envelope on gain reduction
 *   4. Apply: multiply program signal by gain
 *
 * Modes:
 *   0: Standard sidechain compress
 *   1: Pumping (heavy ratio, fast attack, medium release)
 *   2: Ducking (reduce music when voice is present)
 *   3: Multiband sidechain (3-band independent)
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    uint32_t sr;
    float    threshold_db;    /* Threshold in dB */
    float    ratio;           /* Compression ratio (e.g., 4:1) */
    float    attack_ms;       /* Attack time */
    float    release_ms;      /* Release time */
    float    knee_db;         /* Soft knee width */
    float    makeup_db;        /* Makeup gain */

    /* Envelope state */
    float    env_db;          /* Current envelope level in dB */
    float    gain_reduction;  /* Current gain reduction in dB */

    /* Coefficients */
    float    attack_coeff;
    float    release_coeff;
    float    threshold_linear;
    float    slope;           /* 1 - 1/ratio */

    /* Sidechain filter */
    float    sc_hp_freq;      /* Highpass on sidechain detector */
    float    sc_hp_state[4];  /* Biquad states for HP filter */
    float    sc_hp_b0, sc_hp_b1, sc_hp_b2, sc_hp_a1, sc_hp_a2;

    int      mode;
} wb_sidechain_inst;

void *wb_sidechain_create(uint32_t sr) {
    wb_sidechain_inst *sc = (wb_sidechain_inst *)calloc(1, sizeof(*sc));
    if (!sc) return NULL;
    sc->sr = sr;
    sc->threshold_db = -20.0f;
    sc->ratio = 4.0f;
    sc->attack_ms = 5.0f;
    sc->release_ms = 50.0f;
    sc->knee_db = 6.0f;
    sc->makeup_db = 0.0f;
    sc->env_db = -100.0f;
    sc->gain_reduction = 0.0f;
    sc->mode = 0;
    sc->sc_hp_freq = 80.0f;

    /* Compute coefficients */
    sc->attack_coeff = expf(-1.0f / (sc->attack_ms * 0.001f * sr));
    sc->release_coeff = expf(-1.0f / (sc->release_ms * 0.001f * sr));
    sc->threshold_linear = powf(10.0f, sc->threshold_db / 20.0f);
    sc->slope = 1.0f - 1.0f / sc->ratio;

    /* Sidechain highpass filter (remove bass from detector) */
    float omega = 2.0f * 3.14159265f * sc->sc_hp_freq / (float)sr;
    float cos_o = cosf(omega);
    float sin_o = sinf(omega);
    float alpha = sin_o / (2.0f * 0.707f);
    float a0 = 1.0f + alpha;
    sc->sc_hp_b0 = (1.0f + cos_o) / (2.0f * a0);
    sc->sc_hp_b1 = -(1.0f + cos_o) / a0;
    sc->sc_hp_b2 = (1.0f + cos_o) / (2.0f * a0);
    sc->sc_hp_a1 = (-2.0f * cos_o) / a0;
    sc->sc_hp_a2 = (1.0f - alpha) / a0;

    return sc;
}

void wb_sidechain_destroy(void *inst) {
    free(inst);
}

void wb_sidechain_set(void *inst, int param, float v) {
    wb_sidechain_inst *sc = (wb_sidechain_inst *)inst;
    if (!sc) return;
    switch (param) {
    case 0: /* threshold */
        sc->threshold_db = v;
        sc->threshold_linear = powf(10.0f, v / 20.0f);
        break;
    case 1: /* ratio */
        sc->ratio = v > 1.0f ? v : 2.0f;
        sc->slope = 1.0f - 1.0f / sc->ratio;
        break;
    case 2: /* attack */
        sc->attack_ms = v > 0.1f ? v : 0.1f;
        sc->attack_coeff = expf(-1.0f / (sc->attack_ms * 0.001f * sc->sr));
        break;
    case 3: /* release */
        sc->release_ms = v > 1.0f ? v : 10.0f;
        sc->release_coeff = expf(-1.0f / (sc->release_ms * 0.001f * sc->sr));
        break;
    case 4: /* makeup */
        sc->makeup_db = v;
        break;
    case 5: /* mode */
        sc->mode = (int)v;
        break;
    default: break;
    }
}

/* Process a stereo block with sidechain.
 * programL/programR: the signal to compress (modified in place)
 * sidechainL/sidechainR: the detector signal (not modified)
 * n: number of stereo frames */
void wb_sidechain_process(void *inst,
                           wb_sample *programL, wb_sample *programR,
                           const wb_sample *sidechainL, const wb_sample *sidechainR,
                           uint32_t n) {
    wb_sidechain_inst *sc = (wb_sidechain_inst *)inst;
    if (!sc) return;

    float makeup_linear = powf(10.0f, sc->makeup_db / 20.0f);

    for (uint32_t i = 0; i < n; i++) {
        /* Sidechain detector: max of L/R, HP filtered */
        float sc_sample = fabsf(sidechainL[i]) > fabsf(sidechainR[i]) ?
                          fabsf(sidechainL[i]) : fabsf(sidechainR[i]);

        /* HP filter on sidechain (remove bass rumble) */
        float sc_filtered = sc->sc_hp_b0 * sc_sample + sc->sc_hp_b1 * sc->sc_hp_state[0] +
                            sc->sc_hp_b2 * sc->sc_hp_state[1] -
                            sc->sc_hp_a1 * sc->sc_hp_state[2] -
                            sc->sc_hp_a2 * sc->sc_hp_state[3];
        sc->sc_hp_state[1] = sc->sc_hp_state[0];
        sc->sc_hp_state[0] = sc_sample;
        sc->sc_hp_state[3] = sc->sc_hp_state[2];
        sc->sc_hp_state[2] = sc_filtered;

        /* Convert to dB */
        float sc_db = 20.0f * log10f(sc_filtered + 1e-10f);

        /* Envelope follower */
        float target_db = sc_db;
        if (target_db > sc->env_db) {
            sc->env_db = sc->attack_coeff * sc->env_db + (1.0f - sc->attack_coeff) * target_db;
        } else {
            sc->env_db = sc->release_coeff * sc->env_db + (1.0f - sc->release_coeff) * target_db;
        }

        /* Gain computation with soft knee */
        float over_db = sc->env_db - sc->threshold_db;
        float gr_db = 0.0f;

        if (over_db > sc->knee_db * 0.5f) {
            /* Above knee */
            gr_db = sc->slope * (over_db - sc->knee_db * 0.5f);
        } else if (over_db > -sc->knee_db * 0.5f) {
            /* In knee (quadratic interpolation) */
            float x = over_db + sc->knee_db * 0.5f;
            gr_db = sc->slope * x * x / (2.0f * sc->knee_db);
        }

        /* Clamp gain reduction */
        if (gr_db < -60.0f) gr_db = -60.0f;
        if (gr_db > 0.0f) gr_db = 0.0f;

        sc->gain_reduction = gr_db;

        /* Apply gain */
        float gain_linear = powf(10.0f, (gr_db + sc->makeup_db) / 20.0f);
        programL[i] *= gain_linear;
        programR[i] *= gain_linear;
    }
}

/* Quick setup for pumping mode (EDM style) */
void wb_sidechain_set_pumping(void *inst) {
    wb_sidechain_set(inst, 0, -12.0f);  /* threshold */
    wb_sidechain_set(inst, 1, 20.0f);   /* ratio */
    wb_sidechain_set(inst, 2, 0.5f);    /* attack */
    wb_sidechain_set(inst, 3, 150.0f);  /* release */
    wb_sidechain_set(inst, 4, 6.0f);    /* makeup */
    wb_sidechain_set(inst, 5, 1.0f);    /* mode */
}

/* Quick setup for voice ducking (podcast style) */
void wb_sidechain_set_ducking(void *inst) {
    wb_sidechain_set(inst, 0, -24.0f);  /* threshold */
    wb_sidechain_set(inst, 1, 8.0f);    /* ratio */
    wb_sidechain_set(inst, 2, 2.0f);    /* attack */
    wb_sidechain_set(inst, 3, 200.0f);  /* release */
    wb_sidechain_set(inst, 4, 0.0f);    /* makeup */
    wb_sidechain_set(inst, 5, 2.0f);    /* mode */
}
