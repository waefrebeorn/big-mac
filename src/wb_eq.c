/* wb_eq.c — 4-band parametric EQ (zero-delay RBJ biquad cascade).
 * Bands: lowshelf, two peaking, highshelf. Pure C11, matches the engine's
 * 44.1k sample rate. Each band is a transposed biquad updated per block.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wb_unit.h"

/* per-band biquad state */
typedef struct {
    float b0,a1,a2,b1,b2;   /* feedforward/feedback coeffs */
    float x1,x2,y1,y2;      /* state (transposed direct-form II) */
} biquad;

typedef struct {
    uint32_t sr;
    struct { float freq; float gain; float q; int type; } band[4];
    biquad bq[4];
} eq_inst;

void *wb_eq_create(uint32_t sr);
void  wb_eq_destroy(void *inst);
void  wb_eq_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

static void biquad_init(biquad *bq, float b0,float b1,float b2,float a1,float a2) {
    bq->b0=b0; bq->b1=b1; bq->b2=b2; bq->a1=a1; bq->a2=a2;
    bq->x1=bq->x2=bq->y1=bq->y2=0;
}
static float biquad_tick(biquad *bq, float x) {
    float y = bq->b0*x + bq->b1*bq->x1 + bq->b2*bq->x2 - bq->a1*bq->y1 - bq->a2*bq->y2;
    bq->x2=bq->x1; bq->x1=x;
    bq->y2=bq->y1; bq->y1=y;
    return y;
}

static void eq_update_band(eq_inst *e, int i) {
    const float sr = e->sr;
    float f = e->band[i].freq / sr;
    if (f > 0.49f) f = 0.49f;
    float w0 = 2.0f*3.14159265f*f;
    float g = e->band[i].gain;
    float q = e->band[i].q;
    float alpha = sin(w0)/(2.0f*q);
    float b0,b1,b2,a0,a1,a2;
    if (e->band[i].type == 0) { /* lowshelf */
        float A = sqrtf(powf(10.0f, g/20.0f));
        float s = sin(w0);
        float c = cos(w0);
        b0=    A*((A+1) - (A-1)*c + 2*sqrtf(A)*s);
        b1=  2*A*((A-1) - (A+1)*c);
        b2=    A*((A+1) - (A-1)*c - 2*sqrtf(A)*s);
        a0=      (A+1) + (A-1)*c + 2*A*sqrtf(A)*s;
        a1=    2*((A-1) - (A+1)*c);
        a2=      (A+1) + (A-1)*c - 2*A*sqrtf(A)*s;
    } else if (e->band[i].type == 3) { /* highshelf */
        float A = sqrtf(powf(10.0f, g/20.0f));
        float s = sin(w0);
        float c = cos(w0);
        b0=    A*((A+1) + (A-1)*c + 2*sqrtf(A)*s);
        b1= -2*A*((A-1) + (A+1)*c);
        b2=    A*((A+1) + (A-1)*c - 2*sqrtf(A)*s);
        a0=      (A+1) - (A-1)*c + 2*A*sqrtf(A)*s;
        a1=   -2*((A-1) + (A+1)*c);
        a2=      (A+1) - (A-1)*c - 2*A*sqrtf(A)*s;
    } else { /* peaking */
        b0=  1.0f + alpha*g;
        b1=  2.0f*(-(1.0f-cos(w0)));
        b2=  1.0f - alpha*g;
        a0=  1.0f + alpha/g;
        a1=  2.0f*(-(1.0f-cos(w0)));
        a2=  1.0f - alpha/g;
    }
    float inv = 1.0f/a0;
    biquad_init(&e->bq[i], b0*inv, b1*inv, b2*inv, a1*inv, a2*inv);
}

void *wb_eq_create(uint32_t sr) {
    eq_inst *e = calloc(1, sizeof(*e));
    e->sr = sr;
    /* 4 bands: 100Hz lowshelf, 400Hz peak, 2kHz peak, 8kHz highshelf */
    e->band[0].freq=100; e->band[0].gain=0; e->band[0].q=0.7f; e->band[0].type=0;
    e->band[1].freq=400; e->band[1].gain=0; e->band[1].q=1.0f; e->band[1].type=1;
    e->band[2].freq=2000;e->band[2].gain=0; e->band[2].q=1.0f; e->band[2].type=1;
    e->band[3].freq=8000; e->band[3].gain=0; e->band[3].q=0.7f; e->band[3].type=3;
    for (int i=0;i<4;i++) eq_update_band(e,i);
    return e;
}

void wb_eq_destroy(void *inst) { free(inst); }

void wb_eq_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    eq_inst *e = inst;
    for (uint32_t i=0;i<n;i++) {
        float l=L[i], r=R[i];
        for (int b=0;b<4;b++){ l=biquad_tick(&e->bq[b],l); r=biquad_tick(&e->bq[b],r); }
        L[i]=l; R[i]=r;
    }
}

/* ---- registration ------------------------------------------------------ */
static void *u_eq_create(uint32_t sr){ return wb_eq_create(sr); }
static void u_eq_destroy(void *i){ wb_eq_destroy(i); }
static void u_eq_process(void *i, wb_sample *L, wb_sample *R, uint32_t n){ wb_eq_process(i,L,R,n); }
static const char *u_eq_id(void){ return "eq"; }
static const wb_unit_vtable u_eq_vt = {
    u_eq_id, u_eq_create, u_eq_destroy, u_eq_process, 0, 0,0,0 };
static const wb_unit u_eq_unit = { &u_eq_vt };
void wb_unit_ensure_eq(void) { static int d=0; if(!d){ wb_unit_register(&u_eq_unit); d=1; } }
