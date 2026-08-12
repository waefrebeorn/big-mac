/* wb_comp.c — mono/stereo compressor/limiter.
 * Feed-forward VCA-style compressor with soft-knee, attack/release smoothing.
 */

#include <stdlib.h>
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
        float peak = fabsf(L[i]);
        float pr = fabsf(R[i]);
        if (pr > peak) peak = pr;
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
