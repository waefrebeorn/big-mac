/* wb_modulation.c — unified modulation matrix (sources + routing).
 *
 * Sources evaluated per audio block; each route scales a source and writes the
 * resulting normalized value to a destination parameter. Decoupled from the
 * engine via a setter callback so this file stays pure C11 with no engine deps.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus_modulation.h"

#define WB_MOD_MAX_SRC   64
#define WB_MOD_MAX_ROUTE 256

struct wb_mod_matrix {
    wb_mod_src  *srcs[WB_MOD_MAX_SRC];
    int          src_count;
    wb_mod_route routes[WB_MOD_MAX_ROUTE];
    int          route_count;
};

/* ---- source lifecycle -------------------------------------------------- */

wb_mod_src *wb_mod_src_create(wb_mod_src_type type) {
    wb_mod_src *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->type = type;
    s->rate = 1.0f;
    s->depth = 1.0f;
    s->phase = 0.0f;
    s->a = 0.01f; s->d = 0.1f; s->s = 0.7f; s->r = 0.2f;
    for (int i = 0; i < 8; i++) s->step[i] = 0.0f;
    s->step_idx = 0;
    s->enabled = 1;
    return s;
}

void wb_mod_src_destroy(wb_mod_src *s) {
    free(s);
}

/* ---- per-source evaluation --------------------------------------------- */

static float eval_lfo(wb_mod_src *s, uint32_t frames, float sr) {
    /* advance phase by rate Hz over this block; output sine */
    float inc = (s->rate / sr) * frames;
    s->phase += inc;
    if (s->phase >= 1.0f) s->phase -= (float)(int)(s->phase);
    float v = sinf(s->phase * 2.0f * (float)M_PI);   /* -1..1 */
    return v * s->depth;
}

static float eval_env(wb_mod_src *s, uint32_t frames, float sr) {
    /* looping ADSR over one cycle of length 1/rate seconds */
    float inc = (s->rate / sr) * frames;
    s->phase += inc;
    if (s->phase >= 1.0f) s->phase -= (float)(int)(s->phase);
    float p = s->phase;
    float out;
    /* normalized segment boundaries from a/d/s/r (s+r must fit in cycle) */
    float a = s->a, d = s->d, slev = s->s, r = s->r;
    /* clamp so a+d+r <= 1, sustain fills the rest */
    if (a + d + r > 1.0f) { float k = 1.0f / (a + d + r); a*=k; d*=k; r*=k; }
    float t_s = a + d;
    float t_r = t_s + (1.0f - a - d - r);  /* start of release */
    if (p < a)            out = (a > 0) ? (p / a) : 1.0f;
    else if (p < t_s)     out = 1.0f - (1.0f - slev) * ((p - a) / (d > 0 ? d : 1e-6f));
    else if (p < t_r)     out = slev;
    else                  out = slev * (1.0f - (p - t_r) / (r > 0 ? r : 1e-6f));
    return (2.0f * out - 1.0f) * s->depth;  /* map 0..1 -> -1..1 */
}

static float eval_step(wb_mod_src *s, uint32_t frames, float sr) {
    /* advance one step every (1/rate) seconds */
    float inc = (s->rate / sr) * frames;
    s->phase += inc;
    if (s->phase >= 1.0f) {
        s->phase -= (float)(int)(s->phase);
        s->step_idx = (s->step_idx + 1) & 7;
    }
    float v = s->step[s->step_idx];   /* -1..1 */
    return v * s->depth;
}

float wb_mod_src_eval(wb_mod_src *s, uint32_t frames, float sr) {
    if (!s || !s->enabled || sr <= 0) return 0.0f;
    switch (s->type) {
        case WB_MOD_LFO:  return eval_lfo(s, frames, sr);
        case WB_MOD_ENV:  return eval_env(s, frames, sr);
        case WB_MOD_STEP: return eval_step(s, frames, sr);
        default:          return 0.0f;
    }
}

/* ---- matrix ------------------------------------------------------------ */

wb_mod_matrix *wb_mod_matrix_create(void) {
    return calloc(1, sizeof(wb_mod_matrix));
}

void wb_mod_matrix_destroy(wb_mod_matrix *m) {
    if (!m) return;
    for (int i = 0; i < m->src_count; i++)
        wb_mod_src_destroy(m->srcs[i]);
    free(m);
}

int wb_mod_matrix_add_src(wb_mod_matrix *m, wb_mod_src *s) {
    if (!m || !s || m->src_count >= WB_MOD_MAX_SRC) return -1;
    m->srcs[m->src_count] = s;
    return m->src_count++;
}

int wb_mod_matrix_add_route(wb_mod_matrix *m, const wb_mod_route *r) {
    if (!m || !r || m->route_count >= WB_MOD_MAX_ROUTE) return -1;
    m->routes[m->route_count] = *r;
    return m->route_count++;
}

void wb_mod_matrix_clear(wb_mod_matrix *m) {
    if (!m) return;
    for (int i = 0; i < m->src_count; i++)
        wb_mod_src_destroy(m->srcs[i]);
    m->src_count = 0;
    m->route_count = 0;
}

void wb_mod_matrix_eval(wb_mod_matrix *m, uint32_t frames, float sample_rate,
                        wb_mod_setter setter, void *ctx) {
    if (!m || !setter) return;
    for (int i = 0; i < m->route_count; i++) {
        wb_mod_route *r = &m->routes[i];
        if (!r->enabled) continue;
        if (r->src < 0 || r->src >= m->src_count) continue;
        float out = wb_mod_src_eval(m->srcs[r->src], frames, sample_rate);  /* -1..1 */
        float value = r->base + out * r->amount;                            /* 0..1 */
        if (value < 0.0f) value = 0.0f;
        else if (value > 1.0f) value = 1.0f;
        setter(ctx, r->track, r->slot, r->param, value);
    }
}
