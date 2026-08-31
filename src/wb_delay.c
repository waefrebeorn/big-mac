/* wb_delay.c — stereo delay / echo with feedback, lowpass in loop. */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus.h"

typedef struct wb_delay_inst {
    uint32_t sr;
    wb_sample *bufL, *bufR;
    uint32_t cap;       /* allocated samples */
    uint32_t pos;
    float time_ms;
    float feedback;
    float mix;          /* wet/dry */
    float lp_state;
} wb_delay_inst;

void *wb_delay_create(uint32_t sr);
void  wb_delay_destroy(void *inst);
void  wb_delay_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

void *wb_delay_create(uint32_t sr) {
    wb_delay_inst *d = calloc(1, sizeof(*d));
    d->sr = sr;
    d->cap = (uint32_t)(sr * 2.0f);   /* up to 2s delay */
    d->bufL = calloc(d->cap, sizeof(wb_sample));
    d->bufR = calloc(d->cap, sizeof(wb_sample));
    d->time_ms = 300.0f;
    d->feedback = 0.4f;
    d->mix = 0.3f;
    return d;
}
void wb_delay_destroy(void *inst) {
    wb_delay_inst *d = inst;
    if (d) { free(d->bufL); free(d->bufR); free(d); }
}

/* Parameter setter for macro rack binding.
 * param: 0=time_ms, 1=feedback, 2=mix */
void wb_delay_set(void *inst, int param, float v) {
    wb_delay_inst *d = inst;
    if (!d) return;
    switch (param) {
    case 0: d->time_ms = v; break;
    case 1: d->feedback = v; break;
    case 2: d->mix = v; break;
    default: break;
    }
}

void wb_delay_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_delay_inst *d = inst;
    uint32_t delay_len = (uint32_t)(d->time_ms * 0.001f * d->sr);
    if (delay_len > d->cap) delay_len = d->cap;
    if (delay_len < 1) delay_len = 1;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t read = (d->pos + d->cap - delay_len) % d->cap;
        float wetL = d->bufL[read];
        float wetR = d->bufR[read];
        /* write with feedback (with lowpass smoothing) */
        d->lp_state = d->lp_state * 0.7f + (wetL) * 0.3f;
        d->bufL[d->pos] = L[i] + d->feedback * d->lp_state;
        d->bufR[d->pos] = R[i] + d->feedback * wetR;
        d->pos = (d->pos + 1) % d->cap;
        /* mix */
        L[i] = L[i] * (1.0f - d->mix) + wetL * d->mix;
        R[i] = R[i] * (1.0f - d->mix) + wetR * d->mix;
    }
}
/* external per-slot wet mix for delay: save dry, run full process, blend. */
void wb_delay_inplace_wet(void *inst, wb_sample *L, wb_sample *R, uint32_t n, float w) {
    if (w >= 1.0f) { wb_delay_process(inst, L, R, n); return; }
    wb_sample tmpL[4096], tmpR[4096];
    memcpy(tmpL, L, n * sizeof(wb_sample));
    memcpy(tmpR, R, n * sizeof(wb_sample));
    wb_delay_process(inst, L, R, n);
    for (uint32_t i = 0; i < n; i++) {
        L[i] = tmpL[i] * (1.0f - w) + L[i] * w;
        R[i] = tmpR[i] * (1.0f - w) + R[i] * w;
    }
}
