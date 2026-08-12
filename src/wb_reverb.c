/* wb_reverb.c — Schroeder feedback-delay-network reverb (stereo).
 * 4 parallel comb filters into 2 series allpass filters per channel.
 * Zero-third-party, all our math.
 */

#include <stdlib.h>
#include <string.h>
#include "wbus.h"

#define NCOMBS 4
#define NALLPASS 2

typedef struct wb_reverb_inst {
    uint32_t sr;
    float *combs[NCOMBS][2];   /* [comb][channel] */
    uint32_t comb_len[NCOMBS];
    uint32_t comb_pos[NCOMBS][2];
    float *allpass[NALLPASS][2];
    uint32_t ap_len[NALLPASS];
    uint32_t ap_pos[NALLPASS][2];
    float feedback;
    float mix;
} wb_reverb_inst;

void *wb_reverb_create(uint32_t sr);
void  wb_reverb_destroy(void *inst);
void  wb_reverb_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

void *wb_reverb_create(uint32_t sr) {
    wb_reverb_inst *r = calloc(1, sizeof(*r));
    r->sr = sr;
    /* prime comb lengths (ms) — spread so they don't ring together */
    float comb_ms[NCOMBS] = { 29.7f, 37.1f, 41.1f, 43.7f };
    float ap_ms[NALLPASS] = { 5.0f, 1.7f };
    for (int i = 0; i < NCOMBS; i++) {
        r->comb_len[i] = (uint32_t)(comb_ms[i] * 0.001f * sr);
        for (int c = 0; c < 2; c++)
            r->combs[i][c] = calloc(r->comb_len[i], sizeof(float));
    }
    for (int i = 0; i < NALLPASS; i++) {
        r->ap_len[i] = (uint32_t)(ap_ms[i] * 0.001f * sr);
        if (r->ap_len[i] < 1) r->ap_len[i] = 1;
        for (int c = 0; c < 2; c++)
            r->allpass[i][c] = calloc(r->ap_len[i], sizeof(float));
    }
    r->feedback = 0.7f;
    r->mix = 0.25f;
    return r;
}

void wb_reverb_destroy(void *inst) {
    wb_reverb_inst *r = inst;
    if (!r) return;
    for (int i = 0; i < NCOMBS; i++)
        for (int c = 0; c < 2; c++) free(r->combs[i][c]);
    for (int i = 0; i < NALLPASS; i++)
        for (int c = 0; c < 2; c++) free(r->allpass[i][c]);
    free(r);
}

static float comb_tick(float *buf, uint32_t len, uint32_t *pos,
                       float input, float fb) {
    float out = buf[*pos];
    buf[*pos] = input + out * fb;
    *pos = (*pos + 1) % len;
    return out;
}

static float allpass_tick(float *buf, uint32_t len, uint32_t *pos,
                          float input, float g) {
    float out = buf[*pos];
    float delayed = -input + out * g;
    buf[*pos] = input + out * g;
    *pos = (*pos + 1) % len;
    return delayed * g + out - input;
}

void wb_reverb_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_reverb_inst *r = inst;
    for (uint32_t i = 0; i < n; i++) {
        float dryL = L[i], dryR = R[i];
        float sumL = 0, sumR = 0;
        for (int c = 0; c < NCOMBS; c++) {
            sumL += comb_tick(r->combs[c][0], r->comb_len[c], &r->comb_pos[c][0], dryL, r->feedback);
            sumR += comb_tick(r->combs[c][1], r->comb_len[c], &r->comb_pos[c][1], dryR, r->feedback);
        }
        sumL /= NCOMBS;
        sumR /= NCOMBS;
        for (int a = 0; a < NALLPASS; a++) {
            sumL = allpass_tick(r->allpass[a][0], r->ap_len[a], &r->ap_pos[a][0], sumL, 0.5f);
            sumR = allpass_tick(r->allpass[a][1], r->ap_len[a], &r->ap_pos[a][1], sumR, 0.5f);
        }
        /* cross-mix for width */
        float wl = sumL + sumR * 0.2f;
        float wr = sumR + sumL * 0.2f;
        L[i] = dryL * (1.0f - r->mix) + wl * r->mix;
        R[i] = dryR * (1.0f - r->mix) + wr * r->mix;
    }
}
/* external per-slot wet mix for reverb: save dry, run full process, blend. */
void wb_reverb_inplace_wet(void *inst, wb_sample *L, wb_sample *R, uint32_t n, float w) {
    if (w >= 1.0f) { wb_reverb_process(inst, L, R, n); return; }
    wb_sample tmpL[4096], tmpR[4096];
    memcpy(tmpL, L, n * sizeof(wb_sample));
    memcpy(tmpR, R, n * sizeof(wb_sample));
    wb_reverb_process(inst, L, R, n);
    for (uint32_t i = 0; i < n; i++) {
        L[i] = tmpL[i] * (1.0f - w) + L[i] * w;
        R[i] = tmpR[i] * (1.0f - w) + R[i] * w;
    }
}
