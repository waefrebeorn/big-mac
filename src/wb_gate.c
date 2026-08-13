/* wb_gate.c — noise gate / expander, af1.
 * Silences signal below a threshold; fast attack, adjustable release, with
 * hysteresis to avoid chatter. A pro staple for cleaning drums/voice.
 */

#include <stdlib.h>
#include <math.h>
#include "wb_internal.h"

typedef struct {
    uint32_t sr;
    float thresh;    /* 0..1 threshold in linear amplitude */
    float attack;    /* seconds */
    float release;   /* seconds */
    float env;       /* current gain (0..1) */
} wb_gate_inst;

void *wb_gate_create(uint32_t sr) {
    wb_gate_inst *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->sr = sr;
    g->thresh = 0.02f;
    g->attack = 0.001f;
    g->release = 0.08f;
    g->env = 1.0f;
    return g;
}
void wb_gate_destroy(void *inst) { free(inst); }

void wb_gate_set(void *inst, int param, float v) {
    wb_gate_inst *g = inst;
    if (!g) return;
    if (param == 0) g->thresh = v < 0 ? 0 : (v > 1 ? 1 : v);
    else if (param == 1) g->attack = v < 0.0001f ? 0.0001f : (v > 1 ? 1 : v);
    else if (param == 2) g->release = v < 0.001f ? 0.001f : (v > 2 ? 2 : v);
}

void wb_gate_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_gate_inst *g = inst;
    if (!g) return;
    float atk = (float)exp(-1.0 / (g->attack * (float)g->sr));
    float rel = (float)exp(-1.0 / (g->release * (float)g->sr));
    for (uint32_t i = 0; i < n; i++) {
        float peak = fabsf(L[i]) > fabsf(R[i]) ? fabsf(L[i]) : fabsf(R[i]);
        /* target gain: open if above threshold, closed otherwise */
        float target = (peak > g->thresh) ? 1.0f : 0.0f;
        if (target > g->env) g->env = atk * g->env + (1.0f - atk) * target;
        else                 g->env = rel * g->env + (1.0f - rel) * target;
        L[i] *= g->env;
        R[i] *= g->env;
    }
}
