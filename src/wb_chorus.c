/* wb_chorus.c — stereo chorus + flanger-capable delay-line modulation.
 * One or more short delay lines with a modulated read tap produce the
 * characteristic chorused/widened output. Pure C11.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wb_unit.h"

#define TWO_PI 6.2831853071795864769

typedef struct {
    uint32_t sr;
    float *lbuf, *rbuf;
    uint32_t bufsize;
    uint32_t pos;
    float delay_ms;   /* base delay 1..30ms */
    float width_ms;   /* modulation depth */
    float rate_hz;    /* LFO speed */
    float lfo;
    float mix;        /* 0=dry .. 1=wet */
    float fb;         /* feedback */
} chorus_inst;

void *wb_chorus_create(uint32_t sr);
void  wb_chorus_destroy(void *inst);
void  wb_chorus_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

void *wb_chorus_create(uint32_t sr) {
    chorus_inst *c = calloc(1, sizeof(*c));
    c->sr = sr;
    c->delay_ms = 7.0f;
    c->width_ms  = 5.0f;
    c->rate_hz   = 0.4f;
    c->mix = 0.7f;
    c->fb = 0.2f;
    c->bufsize = (uint32_t)(50.0 * 0.001 * sr); /* 50ms headroom */
    if (c->bufsize < 16) c->bufsize = 16;
    c->lbuf = calloc(c->bufsize, sizeof(float));
    c->rbuf = calloc(c->bufsize, sizeof(float));
    return c;
}

void wb_chorus_destroy(void *inst) {
    chorus_inst *c = inst;
    if (!c) return;
    free(c->lbuf); free(c->rbuf); free(c);
}

void wb_chorus_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    chorus_inst *c = inst;
    double sr = c->sr;
    for (uint32_t i = 0; i < n; i++) {
        /* advance LFO for left/right independently (stereo spread) */
        c->lfo += TWO_PI * c->rate_hz / sr;
        if (c->lfo > TWO_PI) c->lfo -= TWO_PI;
        double lfoL = sin(c->lfo);
        double lfoR = sin(c->lfo + TWO_PI/3.0);
        double dly = (c->delay_ms + lfoL * c->width_ms) * 0.001;
        double rdly = (c->delay_ms + lfoR * c->width_ms) * 0.001;
        uint32_t di  = (uint32_t)(dly  * sr);
        uint32_t rdi = (uint32_t)(rdly * sr);
        if (di  >= c->bufsize) di  = c->bufsize-1;
        if (rdi >= c->bufsize) rdi = c->bufsize-1;
        uint32_t readL = (c->pos + c->bufsize - di)  % c->bufsize;
        uint32_t readR = (c->pos + c->bufsize - rdi) % c->bufsize;
        float wl = c->lbuf[readL];
        float wr = c->rbuf[readR];
        /* feedback into delay line */
        c->lbuf[c->pos] = L[i] + wl * c->fb;
        c->rbuf[c->pos] = R[i] + wr * c->fb;
        float wetL = wl; float wetR = wr;
        L[i] = L[i] * (1.0f - c->mix) + wetL * c->mix;
        R[i] = R[i] * (1.0f - c->mix) + wetR * c->mix;
        c->pos = (c->pos + 1) % c->bufsize;
    }
}

/* ---- wb_unit registration ----------------------------------------------- */
static void *u_cc_create(uint32_t sr){ return wb_chorus_create(sr); }
static void u_cc_destroy(void *i){ wb_chorus_destroy(i); }
static void u_cc_process(void *i, wb_sample *L, wb_sample *R, uint32_t n){ wb_chorus_process(i,L,R,n); }
static const char *u_cc_id(void){ return "chorus"; }
static int u_cc_has(const void *i, const char *name){
    (void)i;
    return !strcmp(name,"mix") || !strcmp(name,"rate") || !strcmp(name,"width") || !strcmp(name,"fb");
}
static void u_cc_set(void *i, const char *name, float v){
    chorus_inst *c = i;
    if (!strcmp(name,"mix")){ c->mix = v; }
    else if (!strcmp(name,"rate")){ c->rate_hz = 0.1f + v*4.9f; }
    else if (!strcmp(name,"width")){ c->width_ms = v*25.0f; }
    else if (!strcmp(name,"fb")){ c->fb = v*0.9f; }
}
static float u_cc_get(const void *i, const char *name){
    const chorus_inst *c = i;
    if (!strcmp(name,"mix")) return c->mix;
    if (!strcmp(name,"rate")) return (c->rate_hz-0.1f)/4.9f;
    if (!strcmp(name,"width")) return c->width_ms/25.0f;
    if (!strcmp(name,"fb")) return c->fb/0.9f;
    return 0;
}
static const wb_unit_vtable u_cc_vt = {
    u_cc_id, u_cc_create, u_cc_destroy, u_cc_process, 0,
    u_cc_has, u_cc_set, u_cc_get };
static const wb_unit u_cc_unit = { &u_cc_vt };

void wb_unit_ensure_chorus(void) { static int d=0; if(!d){ wb_unit_register(&u_cc_unit); d=1; } }
