/* wb_comp.c — mono/stereo compressor/limiter.
 * Feed-forward VCA-style compressor with soft-knee, attack/release smoothing.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus.h"

typedef struct wb_comp_inst {
    uint32_t sr;
    float threshold_db;
    float ratio;
    float knee;
    float attack_ms;
    float release_ms;
    float makeup_db;
    float env;   /* smoothed gain reduction envelope */
    float max_peak; /* for peak detection */
    /* sidechain key input: external signal driving the compressor envelope.
     * key_bufL/R hold the last key block; key_active=1 means use key as
     * envelope source instead of program material. */
    wb_sample key_bufL[WB_MAX_BLOCK];
    wb_sample key_bufR[WB_MAX_BLOCK];
    int       key_active;
} wb_comp_inst;

void *wb_comp_create(uint32_t sr);
void  wb_comp_destroy(void *inst);
void  wb_comp_set(void *inst, int param, float v);
void  wb_comp_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

void *wb_comp_create(uint32_t sr) {
    wb_comp_inst *c = calloc(1, sizeof(*c));
    c->sr = sr;
    c->threshold_db = -12.0f;
    c->ratio = 4.0f;
    c->knee = 6.0f;
    c->attack_ms = 5.0f;
    c->release_ms = 120.0f;
    c->makeup_db = 0.0f;
    return c;
}
void wb_comp_destroy(void *inst) { free(inst); }

static float db_to_lin(float db) { return (float)pow(10.0, db / 20.0); }
static float lin_to_db(float v) { return 20.0f * (float)log10(v + 1e-9f); }

void wb_comp_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_comp_inst *c = inst;
    float thresh = c->threshold_db;
    float slope = 1.0f / (c->ratio > 0 ? c->ratio : 4.0f);
    float knee = c->knee;
    float att = (float)exp(-1.0 / (c->attack_ms * 0.001 * c->sr));
    float rel = (float)exp(-1.0 / (c->release_ms * 0.001 * c->sr));
    float makeup = db_to_lin(c->makeup_db);

    for (uint32_t i = 0; i < n; i++) {
        /* envelope source: sidechain key buffer when active, else program audio */
        float peak;
        if (c->key_active) {
            float kl = fabsf(c->key_bufL[i]);
            float kr = fabsf(c->key_bufR[i]);
            peak = kl > kr ? kl : kr;
        } else {
            float pl = fabsf(L[i]);
            float pr = fabsf(R[i]);
            peak = pl > pr ? pl : pr;
        }
        float db = lin_to_db(peak);
        float gain_db = 0.0f;
        /* soft knee compression */
        if (db > (thresh - knee * 0.5f) && db < (thresh + knee * 0.5f)) {
            float x = db - thresh;
            gain_db = x * x / (2.0f * knee * slope) * (slope - 1.0f);
            /* smooth knee approx */
            gain_db = (db - thresh) * (slope - 1.0f) + (x*x/(2*knee))*(1-slope);
        } else if (db >= (thresh + knee * 0.5f)) {
            gain_db = (db - thresh) * (slope - 1.0f);
        }
        float target_gain = db_to_lin(gain_db);
        /* envelope follower (attack fast, release slow) */
        if (target_gain < c->env) c->env = att * c->env + (1.0f - att) * target_gain;
        else                      c->env = rel * c->env + (1.0f - rel) * target_gain;
        float g = c->env * makeup;
        L[i] *= g;
        R[i] *= g;
    }
}

/* stub registrations to satisfy the ABI (the engine wires comp inline for now) */
void wb_comp_set(void *inst, int param, float v) {
    wb_comp_inst *c = inst;
    switch (param) {
    case 0: c->threshold_db = v; break;
    case 1: c->ratio = v; break;
    case 2: c->makeup_db = v; break;
    default: break;
    }
}

/* sidechain key input: feed an external signal to drive the compressor
 * envelope. The key signal is summed to mono and stored in key_bufL/R;
 * key_active=1 means use key as envelope source instead of program material
 * (sidechain compression / ducking). */
void wb_comp_set_key(void *inst, const wb_sample *keyL, const wb_sample *keyR,
                     uint32_t n) {
    wb_comp_inst *c = inst;
    if (n == 0) { c->key_active = 0; return; }
    c->key_active = 1;
    for (uint32_t i = 0; i < n && i < WB_MAX_BLOCK; i++) {
        c->key_bufL[i] = keyL[i];
        c->key_bufR[i] = keyR[i];
    }
}

/* sidechain key process: use the key signal as the envelope source,
 * applying the resulting gain to the program material (L/R). */
void wb_comp_process_key(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_comp_inst *c = inst;
    float thresh = c->threshold_db;
    float slope = 1.0f / (c->ratio > 0 ? c->ratio : 4.0f);
    float knee = c->knee;
    float att = (float)exp(-1.0 / (c->attack_ms * 0.001 * c->sr));
    float rel = (float)exp(-1.0 / (c->release_ms * 0.001 * c->sr));
    float makeup = db_to_lin(c->makeup_db);

    for (uint32_t i = 0; i < n; i++) {
        float kl = fabsf(c->key_bufL[i]);
        float kr = fabsf(c->key_bufR[i]);
        float peak = kl > kr ? kl : kr;
        float db = lin_to_db(peak);
        float gain_db = 0.0f;
        /* soft knee compression on the key signal */
        if (db > (thresh - knee * 0.5f) && db < (thresh + knee * 0.5f)) {
            float x = db - thresh;
            gain_db = x * x / (2.0f * knee * slope) * (slope - 1.0f);
            gain_db = (db - thresh) * (slope - 1.0f) + (x*x/(2*knee))*(1-slope);
        } else if (db >= (thresh + knee * 0.5f)) {
            gain_db = (db - thresh) * (slope - 1.0f);
        }
        float target_gain = db_to_lin(gain_db);
        if (target_gain < c->env) c->env = att * c->env + (1.0f - att) * target_gain;
        else                      c->env = rel * c->env + (1.0f - rel) * target_gain;
        float g = c->env * makeup;
        L[i] *= g;
        R[i] *= g;
    }
}

/* external per-slot wet mix: save dry, run full compressor, blend.
 * w=1.0 -> fully compressed; w=0.0 -> fully dry (no-op save). */
void wb_comp_inplace_wet(void *inst, wb_sample *L, wb_sample *R, uint32_t n, float w) {
    if (w >= 1.0f) { wb_comp_process(inst, L, R, n); return; }
    wb_sample tmpL[4096], tmpR[4096];
    memcpy(tmpL, L, n * sizeof(wb_sample));
    memcpy(tmpR, R, n * sizeof(wb_sample));
    wb_comp_process(inst, L, R, n);
    for (uint32_t i = 0; i < n; i++) {
        L[i] = tmpL[i] * (1.0f - w) + L[i] * w;
        R[i] = tmpR[i] * (1.0f - w) + R[i] * w;
    }
}
