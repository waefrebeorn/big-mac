/* wb_saturation.c — analog-style saturation (waveshaper), af1.
 * tanh soft-clip driven by a drive control, with makeup output. Adds the
 * "analog-esque glow" parallel-saturation technique relies on.
 */

#include <stdlib.h>
#include <math.h>
#include "wb_internal.h"

typedef struct {
    uint32_t sr;
    float drive;     /* 0..1 -> 1x..8x input gain */
    float out;       /* 0..1 -> 0..1.5 makeup */
    float lastL, lastR;
} wb_sat_inst;

void *wb_sat_create(uint32_t sr) {
    wb_sat_inst *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->sr = sr;
    s->drive = 0.3f;
    s->out = 0.7f;
    return s;
}
void wb_sat_destroy(void *inst) { free(inst); }

void wb_sat_set(void *inst, int param, float v) {
    wb_sat_inst *s = inst;
    if (!s) return;
    if (param == 0) s->drive = v < 0 ? 0 : (v > 1 ? 1 : v);
    else if (param == 1) s->out = v < 0 ? 0 : (v > 1 ? 1 : v);
}

void wb_sat_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_sat_inst *s = inst;
    if (!s) return;
    float gain = 1.0f + s->drive * 7.0f;       /* 1x .. 8x */
    float makeup = s->out * 1.5f;              /* 0 .. 1.5 */
    for (uint32_t i = 0; i < n; i++) {
        float l = L[i] * gain;
        float r = R[i] * gain;
        /* tanh soft saturation; 2x oversample-ish by averaging with prev */
        float sl = tanhf(l);
        float sr = tanhf(r);
        L[i] = (0.5f * (sl + s->lastL)) * makeup;
        R[i] = (0.5f * (sr + s->lastR)) * makeup;
        s->lastL = sl;
        s->lastR = sr;
    }
}
