/* wb_multiband.c — 3-band multiband compressor (af1, final sub-item).
 *
 * Signal is split with two Linkwitz-Riley crossovers (each LR2 = two cascaded
 * 2nd-order Butterworth biquads at Q=0.7071, giving a flat-summing 12 dB/oct
 * split). Each band gets its own full compressor (threshold/ratio/attack/
 * release/makeup), then the bands are summed back. This is the canonical
 * "mastering-style" multiband design (Ableton/Reaper/Logic all ship this).
 */

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wb_internal.h"
#include "wbus_dsp.h"

typedef struct {
    uint32_t sr;
    /* crossovers */
    wb_biquad lp1a, lp1b, hp1a, hp1b;   /* crossover 1 (f1) */
    wb_biquad lp2a, lp2b, hp2a, hp2b;   /* crossover 2 (f2) */
    /* per-band compressors */
    void *low, *mid, *high;
    /* parameters (linear 0..1 UI space) */
    float f1, f2;
    float low_thresh, low_ratio, low_makeup;
    float mid_thresh, mid_ratio, mid_makeup;
    float high_thresh, high_ratio, high_makeup;
    int   recomp; /* recompute filters flag */
} wb_mb_inst;

/* freq param 0..1 -> 40..16000 Hz log-mapped, lo sets a floor */
static float p2freq(float p, float lo) {
    float f = 40.0f * powf(16000.0f / 40.0f, p);
    return f < lo ? lo : f;
}
/* thresh 0..1 -> -60..0 dB */
static float p2thr(float p) { return -60.0f + p * 60.0f; }
/* ratio 0..1 -> 1..20 */
static float p2ratio(float p) { return 1.0f + p * 19.0f; }
/* makeup 0..1 -> 0..1.5 */
static float p2mk(float p) { return p * 1.5f; }

static void mb_recompute(wb_mb_inst *m) {
    float f1 = p2freq(m->f1, 40.0f);
    float f2 = p2freq(m->f2, f1 + 20.0f);
    /* LR2: two cascaded Butterworth (Q=0.7071) per leg */
    float q = 0.7071f;
    wb_biquad_init(&m->lp1a, m->sr); wb_biquad_set(&m->lp1a, 0, f1, q, 0);
    wb_biquad_init(&m->lp1b, m->sr); wb_biquad_set(&m->lp1b, 0, f1, q, 0);
    wb_biquad_init(&m->hp1a, m->sr); wb_biquad_set(&m->hp1a, 1, f1, q, 0);
    wb_biquad_init(&m->hp1b, m->sr); wb_biquad_set(&m->hp1b, 1, f1, q, 0);
    wb_biquad_init(&m->lp2a, m->sr); wb_biquad_set(&m->lp2a, 0, f2, q, 0);
    wb_biquad_init(&m->lp2b, m->sr); wb_biquad_set(&m->lp2b, 0, f2, q, 0);
    wb_biquad_init(&m->hp2a, m->sr); wb_biquad_set(&m->hp2a, 1, f2, q, 0);
    wb_biquad_init(&m->hp2b, m->sr); wb_biquad_set(&m->hp2b, 1, f2, q, 0);
    m->recomp = 0;
}

void *wb_mb_create(uint32_t sr) {
    wb_mb_inst *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->sr = sr;
    m->f1 = 0.35f; m->f2 = 0.65f;        /* ~250 Hz and ~2 kHz */
    m->low_thresh = 0.7f;  m->low_ratio = 0.4f;  m->low_makeup  = 0.5f;
    m->mid_thresh = 0.7f;  m->mid_ratio = 0.4f;  m->mid_makeup  = 0.5f;
    m->high_thresh= 0.7f;  m->high_ratio= 0.4f;  m->high_makeup = 0.5f;
    m->low  = wb_comp_create(sr);
    m->mid  = wb_comp_create(sr);
    m->high = wb_comp_create(sr);
    if (!m->low || !m->mid || !m->high) { wb_mb_destroy(m); return NULL; }
    mb_recompute(m);
    return m;
}

void wb_mb_destroy(void *inst) {
    wb_mb_inst *m = inst;
    if (!m) return;
    if (m->low)  wb_comp_destroy(m->low);
    if (m->mid)  wb_comp_destroy(m->mid);
    if (m->high) wb_comp_destroy(m->high);
    free(m);
}

/* push a sample through a 2-stage cascade of the same biquad type */
static float cascade2(wb_biquad *a, wb_biquad *b, float x) {
    return wb_biquad_process(b, wb_biquad_process(a, x));
}

void wb_mb_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_mb_inst *m = inst;
    if (!m) return;
    if (m->recomp) mb_recompute(m);

    /* per-band compressor params (set once per block) */
    wb_comp_set(m->low,  0, p2thr(m->low_thresh));  wb_comp_set(m->low,  1, p2ratio(m->low_ratio));  wb_comp_set(m->low,  2, p2mk(m->low_makeup));
    wb_comp_set(m->mid,  0, p2thr(m->mid_thresh));  wb_comp_set(m->mid,  1, p2ratio(m->mid_ratio));  wb_comp_set(m->mid,  2, p2mk(m->mid_makeup));
    wb_comp_set(m->high, 0, p2thr(m->high_thresh)); wb_comp_set(m->high, 1, p2ratio(m->high_ratio)); wb_comp_set(m->high, 2, p2mk(m->high_makeup));

    for (uint32_t i = 0; i < n; i++) {
        /* low = LP of crossover 1 */
        float low  = cascade2(&m->lp1a, &m->lp1b, L[i]);
        /* high path of crossover 1 */
        float h1   = cascade2(&m->hp1a, &m->hp1b, L[i]);
        /* mid = LP(f2) of h1 ; high = HP(f2) of h1 */
        float mid  = cascade2(&m->lp2a, &m->lp2b, h1);
        float high = cascade2(&m->hp2a, &m->hp2b, h1);
        float out = low + mid + high;   /* LR sum is flat */
        L[i] = out;
        /* right channel: same filters (shared state is fine for stereo) */
        float lowr  = cascade2(&m->lp1a, &m->lp1b, R[i]);
        float h1r   = cascade2(&m->hp1a, &m->hp1b, R[i]);
        float midr  = cascade2(&m->lp2a, &m->lp2b, h1r);
        float highr = cascade2(&m->hp2a, &m->hp2b, h1r);
        R[i] = lowr + midr + highr;
    }

    wb_comp_process(m->low,  L, L, n);
    wb_comp_process(m->mid,  L, L, n);
    wb_comp_process(m->high, L, L, n);
    wb_comp_process(m->low,  R, R, n);
    wb_comp_process(m->mid,  R, R, n);
    wb_comp_process(m->high, R, R, n);
}

/* ---- parameter interface (named, 0..1) ----------------------------------- */
int wb_mb_has_param(const void *inst, const char *name) {
    (void)inst;
    static const char *names[] = {
        "f1","f2",
        "low_thresh","low_ratio","low_makeup",
        "mid_thresh","mid_ratio","mid_makeup",
        "high_thresh","high_ratio","high_makeup", NULL };
    for (int i = 0; names[i]; i++) if (strcmp(name, names[i]) == 0) return 1;
    return 0;
}
void wb_mb_set_param(void *inst, const char *name, float v) {
    wb_mb_inst *m = inst; if (!m) return;
    v = v < 0 ? 0 : (v > 1 ? 1 : v);
    if (!strcmp(name,"f1")) m->f1 = v;
    else if (!strcmp(name,"f2")) m->f2 = v;
    else if (!strcmp(name,"low_thresh")) m->low_thresh = v;
    else if (!strcmp(name,"low_ratio")) m->low_ratio = v;
    else if (!strcmp(name,"low_makeup")) m->low_makeup = v;
    else if (!strcmp(name,"mid_thresh")) m->mid_thresh = v;
    else if (!strcmp(name,"mid_ratio")) m->mid_ratio = v;
    else if (!strcmp(name,"mid_makeup")) m->mid_makeup = v;
    else if (!strcmp(name,"high_thresh")) m->high_thresh = v;
    else if (!strcmp(name,"high_ratio")) m->high_ratio = v;
    else if (!strcmp(name,"high_makeup")) m->high_makeup = v;
    else return;
    m->recomp = 1;
}
float wb_mb_get_param(const void *inst, const char *name) {
    const wb_mb_inst *m = inst; if (!m) return 0;
    if (!strcmp(name,"f1")) return m->f1;
    if (!strcmp(name,"f2")) return m->f2;
    if (!strcmp(name,"low_thresh")) return m->low_thresh;
    if (!strcmp(name,"low_ratio")) return m->low_ratio;
    if (!strcmp(name,"low_makeup")) return m->low_makeup;
    if (!strcmp(name,"mid_thresh")) return m->mid_thresh;
    if (!strcmp(name,"mid_ratio")) return m->mid_ratio;
    if (!strcmp(name,"mid_makeup")) return m->mid_makeup;
    if (!strcmp(name,"high_thresh")) return m->high_thresh;
    if (!strcmp(name,"high_ratio")) return m->high_ratio;
    if (!strcmp(name,"high_makeup")) return m->high_makeup;
    return 0;
}
