/* wb_bitcrush.c — R074 hop 147 (G-SF065): sample-rate decimation +
 * bit-depth reduction for chiptune/lo-fi timbres. Pure C11.
 * Params (normalized 0..1): decim, bits, mix.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wb_unit.h"

typedef struct {
    uint32_t sr;
    float    hold_l, hold_r;   /* sample-and-hold state */
    double   phase;            /* decimator clock phase */
    int      decim;            /* keep 1 of N samples */
    int      bits;             /* effective bit depth 1..16 */
    float    mix;
} crush_inst;

void *wb_crush_create(uint32_t sr);
void  wb_crush_destroy(void *inst);
void  wb_crush_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

static float quantize(float v, int bits) {
    if (bits >= 16) return v;
    if (bits < 1) bits = 1;
    float steps = (float)((1 << bits) - 1);
    float q = floorf((v * 0.5f + 0.5f) * steps + 0.5f) / steps;
    return q * 2.0f - 1.0f;
}

void *wb_crush_create(uint32_t sr) {
    crush_inst *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->sr = sr;
    c->decim = 1;
    c->bits = 16;
    c->mix = 0.0f;   /* bypass until configured */
    return c;
}

void wb_crush_destroy(void *inst) { free(inst); }

void wb_crush_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    crush_inst *c = inst;
    if (!c || c->decim <= 1) {
        if (c && c->bits >= 16 && c->mix <= 0.0f) return;
    }
    for (uint32_t i = 0; i < n; i++) {
        /* decimator clock: sample-and-hold every c->decim frames */
        c->phase += 1.0;
        if (c->phase >= (double)c->decim) {
            c->phase -= (double)c->decim;
            c->hold_l = L[i];
            c->hold_r = R[i];
        }
        float cl = quantize(c->hold_l, c->bits);
        float cr = quantize(c->hold_r, c->bits);
        L[i] = L[i] * (1.0f - c->mix) + cl * c->mix;
        R[i] = R[i] * (1.0f - c->mix) + cr * c->mix;
    }
}

/* ---- unit registration ------------------------------------------------ */
static void *u_bc_create(uint32_t sr){ return wb_crush_create(sr); }
static void u_bc_destroy(void *i){ wb_crush_destroy(i); }
static void u_bc_process(void *i, wb_sample *L, wb_sample *R, uint32_t n){ wb_crush_process(i,L,R,n); }
static const char *u_bc_id(void){ return "bitcrush"; }
static int u_bc_has(const void *i, const char *name){
    (void)i;
    return !strcmp(name,"decim") || !strcmp(name,"bits") || !strcmp(name,"mix");
}
static void u_bc_set(void *i, const char *name, float v){
    crush_inst *c = i;
    if (!strcmp(name,"decim")){          /* 0..1 -> 1..32 taps */
        c->decim = 1 + (int)(v * 31.0f);
        if (c->decim > 64) c->decim = 64;
    }
    else if (!strcmp(name,"bits")){      /* 0..1 -> 1..16 bits */
        c->bits = 1 + (int)(v * 15.0f);
    }
    else if (!strcmp(name,"mix")){ c->mix = v; }
}
static float u_bc_get(const void *i, const char *name){
    const crush_inst *c = i;
    if (!strcmp(name,"decim")) return (float)(c->decim - 1) / 31.0f;
    if (!strcmp(name,"bits"))  return (float)(c->bits - 1) / 15.0f;
    if (!strcmp(name,"mix"))   return c->mix;
    return 0;
}
static const wb_unit_vtable u_bc_vt = {
    u_bc_id, u_bc_create, u_bc_destroy, u_bc_process, 0,
    u_bc_has, u_bc_set, u_bc_get };
static const wb_unit u_bc_unit = { &u_bc_vt };

void wb_unit_ensure_bitcrush(void) { static int d=0; if(!d){ wb_unit_register(&u_bc_unit); d=1; } }
