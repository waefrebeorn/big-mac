/* wb_compositor.c — pull-based RoI/RoD node compositor (R013 D1/D3).
 * Pure C11. Recursive pull with identity skip + edge cache (LRU). */

#include "wbus/wbus_compositor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "wbus/wbus_anim.h"
#include "wbus/wb_ui.h"   /* R073 hop 43: CGI source */

/* ---- G1: global quality-of-service dial (0..1) ----------------------- */
static double g_quality = 1.0;   /* default full quality */
static int    g_backend = WB_RENDER_CPU;  /* G12: CPU authoritative */

void wb_compositor_set_quality(double q) {
    if (q < 0.0) q = 0.0;
    if (q > 1.0) q = 1.0;
    g_quality = q;
}
double wb_compositor_get_quality(void) { return g_quality; }

/* Tile size shrinks with quality: full-res = 1024px tiles, draft = 128px.
 * Smaller tiles spread work and let a slow frame bail/retry a tile. */
int wb_compositor_tile_size(void) {
    return (int)(128.0 + g_quality * (1024.0 - 128.0));
}

/* ---- G12: GPU-offload boundary -------------------------------------- */
void wb_compositor_set_backend(wb_render_backend b) {
    /* CPU path is authoritative; flipping to GPU only marks the boundary
     * where a Metal interop layer would wrap wb_px buffers. */
    g_backend = (b == WB_RENDER_GPU) ? WB_RENDER_GPU : WB_RENDER_CPU;
}
wb_render_backend wb_compositor_get_backend(void) { return (wb_render_backend)g_backend; }
void wb_frame_set_gpu(wb_frame *f, int gpu) { if (f) f->gpu = gpu ? 1 : 0; }
int  wb_frame_get_gpu(const wb_frame *f) { return f ? f->gpu : 0; }

/* ---- frame ------------------------------------------------------------ */
wb_frame *wb_frame_alloc(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    wb_frame *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->w = w; f->h = h;
    f->px = calloc((size_t)w * h, sizeof(wb_px));
    if (!f->px) { free(f); return NULL; }
    return f;
}
void wb_frame_free(wb_frame *f) {
    if (!f) return;
    free(f->px);
    free(f);
}

int wb_roi_clip(int w, int h, int *rx, int *ry, int *rw, int *rh) {
    if (*rx < 0) { *rw += *rx; *rx = 0; }
    if (*ry < 0) { *rh += *ry; *ry = 0; }
    if (*rx + *rw > w) *rw = w - *rx;
    if (*ry + *rh > h) *rh = h - *ry;
    if (*rw <= 0 || *rh <= 0) { *rw = 0; *rh = 0; return 0; }
    return 1;
}

/* ---- node plumbing ---------------------------------------------------- */
wb_node *wb_node_create(wb_node_kind kind, const char *id) {
    wb_node *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->kind = kind;
    n->n_inputs = 0;
    n->inputs = NULL;
    if (id) snprintf(n->id, sizeof(n->id), "%s", id);
    return n;
}

void wb_node_destroy(wb_node *n) {
    if (!n) return;
    if (n->free) n->free(n);
    for (int i = 0; i < n->n_inputs; i++) {
        /* composite/cache own their children; source/effect don't */
        if (n->kind == WB_NODE_COMPOSITE || n->kind == WB_NODE_CACHE)
            wb_node_destroy(n->inputs[i]);
    }
    free(n->inputs);
    free(n->params);
    free(n->param_lanes);
    free(n);
}

/* ---- G11 param bus ---------------------------------------------------- */
int wb_node_add_param(wb_node *n, const char *name, wb_param_track *tr) {
    if (!n || !tr || n->n_params >= 16) return -1;
    n->params = realloc(n->params, (n->n_params + 1) * sizeof(wb_param_track*));
    if (!n->params) return -1;
    n->param_lanes = realloc(n->param_lanes, (n->n_params + 1) * sizeof(wb_automation_lane*));
    if (!n->param_lanes) return -1;
    n->params[n->n_params] = tr;
    n->param_lanes[n->n_params] = NULL;
    if (name) snprintf(n->param_names[n->n_params], sizeof(n->param_names[0]), "%s", name);
    else n->param_names[n->n_params][0] = '\0';
    return n->n_params++;
}

int wb_node_add_param_lane(wb_node *n, const char *name, wb_automation_lane *lane) {
    if (!n || !lane || n->n_params >= 16) return -1;
    n->params = realloc(n->params, (n->n_params + 1) * sizeof(wb_param_track*));
    if (!n->params) return -1;
    n->param_lanes = realloc(n->param_lanes, (n->n_params + 1) * sizeof(wb_automation_lane*));
    if (!n->param_lanes) return -1;
    n->params[n->n_params] = NULL;
    n->param_lanes[n->n_params] = lane;
    if (name) snprintf(n->param_names[n->n_params], sizeof(n->param_names[0]), "%s", name);
    else n->param_names[n->n_params][0] = '\0';
    return n->n_params++;
}

float wb_node_param_value(const wb_node *n, const char *name, double t) {
    if (!n) return 0.0f;
    for (int i = 0; i < n->n_params; i++)
        if (strcmp(n->param_names[i], name) == 0) {
            /* precedence: keyframed track overrides lane (both are the bus) */
            if (n->params[i]) return wb_param_track_value_at(n->params[i], t);
            if (n->param_lanes[i]) return (float)wb_automation_value_at(n->param_lanes[i], t, 0.0);
            return 0.0f;
        }
    return 0.0f;
}

/* ---- generic pull (identity short-circuit + forwarding) -------------- */
wb_frame *wb_node_pull(wb_node *n, double t, int rx, int ry, int rw, int rh) {
    if (!n) return NULL;
    if (!wb_roi_clip(4096, 4096, &rx, &ry, &rw, &rh)) return NULL;
    /* phase 1 = compute (the default for callers that don't two-phase) */
    return n->pull(n, t, rx, ry, rw, rh, 1);
}

/* G3: phase 0 = request/prepare (schedule decodes, no frame yet). Walk the
 * graph requesting inputs so slow sources (decode) can run ahead; the
 * subsequent wb_node_pull (phase 1) then computes. VapourSynth-style
 * arInitial -> arAllFramesReady. */
void wb_node_pull_request(wb_node *n, double t, int rx, int ry, int rw, int rh) {
    if (!n) return;
    if (!wb_roi_clip(4096, 4096, &rx, &ry, &rw, &rh)) return;
    n->pull(n, t, rx, ry, rw, rh, 0);   /* phase 0: request only */
}

/* ---- SOURCE (color producer) ----------------------------------------- */
typedef struct { float r,g,b,a; int w,h; } src_color_t;
static wb_frame *src_color_pull(wb_node *self, double t,
                                int rx, int ry, int rw, int rh, int phase) {
    (void)t;
    if (phase == 0) return NULL;   /* source is always ready; nothing to request */
    src_color_t *s = self->user;
    wb_frame *f = wb_frame_alloc(s->w, s->h);
    if (!f) return NULL;
    f->roi_x = rx; f->roi_y = ry; f->roi_w = rw; f->roi_h = rh;
    for (int y = ry; y < ry + rh; y++)
        for (int x = rx; x < rx + rw; x++) {
            f->px[y*s->w + x].r = s->r;
            f->px[y*s->w + x].g = s->g;
            f->px[y*s->w + x].b = s->b;
            f->px[y*s->w + x].a = s->a;
        }
    return f;
}
wb_node *wb_node_source_color(float r, float g, float b, float a, int w, int h) {
    wb_node *n = wb_node_create(WB_NODE_SOURCE, "src_color");
    if (!n) return NULL;
    src_color_t *s = calloc(1, sizeof(*s));
    s->r=r; s->g=g; s->b=b; s->a=a; s->w=w; s->h=h;
    n->user = s;
    n->pull = src_color_pull;
    return n;
}

/* ---- G3: DECODE SOURCE (simulated async frame decode) ---------------
 * Models a real decoder (FFmpeg) whose read is expensive: phase 0 schedules
 * the decode (sets pending), phase 1 completes it and returns the frame.
 * This is what makes the two-phase contract observable/testable — a plain
 * color source is always ready, but a decode source demonstrates
 * request-before-compute. */
typedef struct { float r,g,b,a; int w,h; int pending; int ready; } dec_t;
static wb_frame *dec_pull(wb_node *self, double t,
                          int rx, int ry, int rw, int rh, int phase) {
    dec_t *d = self->user;
    (void)t;
    if (phase == 0) { d->pending = 1; return NULL; }   /* schedule decode */
    if (!d->pending) d->pending = 1;                    /* lazy request ok */
    d->ready = 1;
    wb_frame *f = wb_frame_alloc(d->w, d->h);
    if (!f) return NULL;
    f->roi_x = rx; f->roi_y = ry; f->roi_w = rw; f->roi_h = rh;
    for (int y = ry; y < ry + rh; y++)
        for (int x = rx; x < rx + rw; x++) {
            f->px[y*d->w + x].r = d->r; f->px[y*d->w + x].g = d->g;
            f->px[y*d->w + x].b = d->b; f->px[y*d->w + x].a = d->a;
        }
    return f;
}
wb_node *wb_node_decode_source(float r, float g, float b, float a, int w, int h) {
    wb_node *n = wb_node_create(WB_NODE_SOURCE, "decode_src");
    if (!n) return NULL;
    dec_t *d = calloc(1, sizeof(*d));
    d->r=r; d->g=g; d->b=b; d->a=a; d->w=w; d->h=h;
    n->user = d;
    n->pull = dec_pull;
    return n;
}
/* query decode-source scheduling state (for tests) */
int wb_node_decode_is_requested(const wb_node *n) {
    if (!n || n->kind != WB_NODE_SOURCE) return 0;
    dec_t *d = n->user; return d ? d->pending : 0;
}
int wb_node_decode_is_ready(const wb_node *n) {
    if (!n || n->kind != WB_NODE_SOURCE) return 0;
    dec_t *d = n->user; return d ? d->ready : 0;
}

/* ---- EFFECT (gain / invert-alpha) ------------------------------------ */
typedef struct { int op; float gain; } eff_t;
static wb_frame *eff_pull(wb_node *self, double t,
                          int rx, int ry, int rw, int rh, int phase) {
    eff_t *e = self->user;
    if (self->n_inputs < 1) return NULL;
    /* G3: request inputs in phase 0 (so upstream decodes can run ahead) */
    if (phase == 0) {
        wb_node_pull_request(self->inputs[0], t, rx, ry, rw, rh);
        return NULL;
    }
    /* identity shortcut: op 0 = bypass */
    if (e->op == 0) {
        wb_frame *in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
        return in;  /* pass through (VirtualDub-style bypass) */
    }
    wb_frame *in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
    if (!in) return NULL;
    in->roi_x = rx; in->roi_y = ry; in->roi_w = rw; in->roi_h = rh;
    /* G11: a keyframed "gain" param overrides the static gain (animates) */
    float gain = e->gain;
    float kv = wb_node_param_value(self, "gain", t);
    if (kv != 0.0f || e->gain == 0.0f) gain = kv;  /* track present => use it */
    for (int y = ry; y < ry + rh; y++)
        for (int x = rx; x < rx + rw; x++) {
            wb_px *p = &in->px[y*in->w + x];
            if (e->op == 1) { p->r*=gain; p->g*=gain; p->b*=gain; }
            else if (e->op == 2) { /* invert alpha matte */
                p->a = 1.0f - p->a;
            }
            else if (e->op == 4) {
                /* R073 hop 42: gaussian blur — two passes of a 3-tap box
                 * (H+V, repeated) approximates a gaussian; radius from the
                 * keyframable "blur" param (0..~8). Operates on the RoI. */
                float rad = wb_node_param_value(self, "blur", t);
                if (rad <= 0.0f) rad = gain;
                int r = (int)(rad * 3.0f);
                if (r < 1) { /* identity when radius < 1 */ }
                else {
                    int W = in->w, H = in->h;
                    wb_frame *tmp = wb_frame_alloc(W, H);
                    if (tmp) {
                        for (int pass = 0; pass < 2; pass++) {
                            wb_frame *srcf = (pass == 0) ? in : tmp;
                            wb_frame *dstf = (pass == 0) ? tmp : in;
                            /* horizontal */
                            for (int y = ry; y < ry + rh; y++)
                                for (int x = rx; x < rx + rw; x++) {
                                    float ar=0,ag=0,ab=0,aa=0; int n=0;
                                    for (int k = -r; k <= r; k++) {
                                        int xx = x + k;
                                        if (xx < 0 || xx >= W) continue;
                                        wb_px *q=&srcf->px[y*W+xx];
                                        ar+=q->r; ag+=q->g; ab+=q->b; aa+=q->a; n++;
                                    }
                                    wb_px *d=&dstf->px[y*W+x];
                                    d->r=ar/n; d->g=ag/n; d->b=ab/n; d->a=aa/n;
                                }
                            /* vertical */
                            for (int y = ry; y < ry + rh; y++)
                                for (int x = rx; x < rx + rw; x++) {
                                    float ar=0,ag=0,ab=0,aa=0; int n=0;
                                    for (int k = -r; k <= r; k++) {
                                        int yy = y + k;
                                        if (yy < 0 || yy >= H) continue;
                                        wb_px *q=&srcf->px[yy*W+x];
                                        ar+=q->r; ag+=q->g; ab+=q->b; aa+=q->a; n++;
                                    }
                                    wb_px *d=&dstf->px[y*W+x];
                                    d->r=ar/n; d->g=ag/n; d->b=ab/n; d->a=aa/n;
                                }
                        }
                        wb_frame_free(tmp);
                    }
                }
            }
            else if (e->op == 6) {
                /* R073 hop 47a: vignette — radial darkening; strength via
                 * keyframable "vig" param, radius fixed at frame corner */
                float vig = wb_node_param_value(self, "vig", t);
                if (vig <= 0.0f) vig = gain;
                float cx2 = in->w * 0.5f, cy2 = in->h * 0.5f;
                float maxd = sqrtf(cx2*cx2 + cy2*cy2);
                for (int y = ry; y < ry + rh; y++)
                    for (int x = rx; x < rx + rw; x++) {
                        wb_px *p = &in->px[y*in->w + x];
                        float dx = x - cx2, dy = y - cy2;
                        float dnorm = sqrtf(dx*dx + dy*dy) / maxd;
                        /* darkening starts past 50% radius */
                        float fall = dnorm < 0.5f ? 0.0f
                                   : (dnorm - 0.5f) / 0.5f;
                        float k = 1.0f - vig * fall * fall;
                        p->r *= k; p->g *= k; p->b *= k;
                    }
            }
            else if (e->op == 8) {
                /* R073 hop 52: primary grade — lift/gamma/gain/saturation,
                 * all keyframable via params of the same names. */
                float lift = wb_node_param_value(self, "lift", t);
                float gam  = wb_node_param_value(self, "gamma", t);
                float gnv  = wb_node_param_value(self, "gain", t);
                float sat  = wb_node_param_value(self, "sat", t);
                if (gam <= 0.0f) gam = 1.0f;
                if (gnv <= 0.0f) gnv = 1.0f;
                if (sat <= 0.0f) sat = e->gain > 0 ? e->gain : 1.0f;
                p->r = (p->r + lift) * gnv;
                p->g = (p->g + lift) * gnv;
                p->b = (p->b + lift) * gnv;
                /* gamma on positive values */
                p->r = powf(p->r > 0 ? p->r : 0, 1.0f / gam);
                p->g = powf(p->g > 0 ? p->g : 0, 1.0f / gam);
                p->b = powf(p->b > 0 ? p->b : 0, 1.0f / gam);
                /* saturation around Rec.709 luma */
                float lum = 0.2126f*p->r + 0.7152f*p->g + 0.0722f*p->b;
                p->r = lum + (p->r - lum)*sat;
                p->g = lum + (p->g - lum)*sat;
                p->b = lum + (p->b - lum)*sat;
            }
            else if (e->op == 7) {
                /* R073 hop 47b: glow — threshold bright pixels, blur them,
                 * screen-add back. Simplified: per-pixel soft-knee bloom on
                 * luminance above the keyframable "glow_thr" param. */
                float thr = wb_node_param_value(self, "glow_thr", t);
                if (thr <= 0.0f) thr = 0.7f;
                for (int y = ry; y < ry + rh; y++)
                    for (int x = rx; x < rx + rw; x++) {
                        wb_px *p = &in->px[y*in->w + x];
                        float lum = 0.2126f*p->r + 0.7152f*p->g
                                  + 0.0722f*p->b;
                        if (lum > thr) {
                            float excess = (lum - thr) / (1.0f - thr);
                            p->r += excess * (p->r) * 0.5f;
                            p->g += excess * (p->g) * 0.5f;
                            p->b += excess * (p->b) * 0.5f;
                        }
                    }
            }
            else if (e->op == 5) {
                /* R073 hop 45: luma key — dark pixels go transparent
                 * (screen-blend style, ideal over black-bg CGI renders).
                 * threshold from keyframable "lum_thr" param. */
                float thr = wb_node_param_value(self, "lum_thr", t);
                if (thr <= 0.0f) thr = gain > 0 ? gain : 0.15f;
                float lum = 0.2126f*p->r + 0.7152f*p->g + 0.0722f*p->b;
                if (lum <= thr)       p->a = 0.0f;
                else if (lum < thr*2) p->a *= (lum - thr) / thr;
            }
            else if (e->op == 3) {
                /* R073 hop 41: chroma key — green-screen removal.
                 * keyed when green dominates red+blue beyond tolerance;
                 * edge softening via partial alpha in the tolerance band. */
                float tol = wb_node_param_value(self, "key_tol", t);
                if (tol <= 0.0f) tol = gain;      /* static fallback */
                if (tol <= 0.0f) tol = 0.15f;
                float gdom = p->g - 0.5f * (p->r + p->b);
                if (gdom >= tol)       p->a = 0.0f;
                else if (gdom > tol*0.5f)
                    p->a *= 1.0f - (gdom - tol*0.5f) / (tol*0.5f);
            }
        }
    return in;
}
wb_node *wb_node_effect(int op, float gain) {
    wb_node *n = wb_node_create(WB_NODE_EFFECT, "effect");
    if (!n) return NULL;
    eff_t *e = calloc(1, sizeof(*e));
    e->op = op; e->gain = gain;
    n->user = e;
    n->pull = eff_pull;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node*));
    return n;
}

/* ---- TRANSFORM (affine scale/pan/rotate, keyframable) ------------------ */
typedef struct { float scale, cx, cy, rot; } tf_t;

static wb_frame *tf_pull(wb_node *self, double t,
                         int rx, int ry, int rw, int rh, int phase) {
    tf_t *tf = self->user;
    if (self->n_inputs < 1) return NULL;
    if (phase == 0) { wb_node_pull_request(self->inputs[0], t, rx, ry, rw, rh); return NULL; }
    /* G11: keyframed params animate the transform */
    float scale = tf->scale, cx = tf->cx, cy = tf->cy, rot = tf->rot;
    float k;
    k = wb_node_param_value(self, "scale", t); if (k != 0.0f) scale = k;
    k = wb_node_param_value(self, "cx",    t); if (k != 0.0f) cx = k;
    k = wb_node_param_value(self, "cy",    t); if (k != 0.0f) cy = k;
    k = wb_node_param_value(self, "rot",   t); if (k != 0.0f) rot = k;

    wb_frame *in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
    if (!in) return NULL;
    wb_frame *out = wb_frame_alloc(in->w, in->h);
    if (!out) { wb_frame_free(in); return NULL; }
    out->roi_x = rx; out->roi_y = ry; out->roi_w = rw; out->roi_h = rh;

    /* pivot in pixel space (normalized cx,cy over the frame) */
    float px = cx * in->w, py = cy * in->h;
    float c = cosf(rot), s = sinf(rot);
    float sc = (scale > 1e-3f) ? scale : 1e-3f;
    for (int y = 0; y < in->h; y++) {
        for (int x = 0; x < in->w; x++) {
            /* translate to pivot, un-rotate, un-scale, translate back */
            float dx = (float)x - px, dy = (float)y - py;
            float sx = (dx * c + dy * s) / sc + px;
            float sy = (-dx * s + dy * c) / sc + py;
            int ix = (int)floorf(sx), iy = (int)floorf(sy);
            wb_px *dst = &out->px[y * in->w + x];
            if (ix >= 0 && ix < in->w && iy >= 0 && iy < in->h) {
                *dst = in->px[iy * in->w + ix];   /* nearest-neighbor sample */
            } else {
                dst->r = dst->g = dst->b = 0.0f; dst->a = 0.0f;  /* outside = transparent */
            }
        }
    }
    wb_frame_free(in);
    return out;
}
wb_node *wb_node_transform(void) {
    wb_node *n = wb_node_create(WB_NODE_EFFECT, "transform");
    if (!n) return NULL;
    tf_t *tf = calloc(1, sizeof(*tf));
    tf->scale = 1.0f; tf->cx = 0.5f; tf->cy = 0.5f; tf->rot = 0.0f;
    n->user = tf;
    n->pull = tf_pull;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node*));
    return n;
}

/* ---- COMPOSITE (alpha over, bottom..top) ----------------------------- */
static wb_frame *comp_pull(wb_node *self, double t,
                           int rx, int ry, int rw, int rh, int phase) {
    if (self->n_inputs < 1) return NULL;
    if (phase == 0) {   /* G3: request all inputs ahead of compute */
        for (int i = 0; i < self->n_inputs; i++)
            wb_node_pull_request(self->inputs[i], t, rx, ry, rw, rh);
        return NULL;
    }
    wb_frame *out = wb_frame_alloc(4096, 4096); /* RoD of composite */
    if (!out) return NULL;
    out->roi_x = rx; out->roi_y = ry; out->roi_w = rw; out->roi_h = rh;
    for (int i = 0; i < self->n_inputs; i++) {
        wb_frame *f = wb_node_pull(self->inputs[i], t, rx, ry, rw, rh);
        if (!f) continue;
        for (int y = ry; y < ry + rh; y++)
            for (int x = rx; x < rx + rw; x++) {
                wb_px s = f->px[y*f->w + x];
                wb_px *d = &out->px[y*out->w + x];
                float a = s.a;
                d->r = s.r*a + d->r*(1-a);
                d->g = s.g*a + d->g*(1-a);
                d->b = s.b*a + d->b*(1-a);
                d->a = a + d->a*(1-a);
            }
        wb_frame_free(f);
    }
    return out;
}
wb_node *wb_node_composite(void) {
    wb_node *n = wb_node_create(WB_NODE_COMPOSITE, "composite");
    if (!n) return NULL;
    n->pull = comp_pull;
    n->n_inputs = 0;
    n->inputs = NULL;
    return n;
}
/* attach an input to a composite (caller keeps ownership of child) */
static void comp_add(wb_node *comp, wb_node *child) {
    comp->inputs = realloc(comp->inputs, (comp->n_inputs+1)*sizeof(wb_node*));
    comp->inputs[comp->n_inputs++] = child;
}

/* ---- CACHE (auto edge memoization, bounded LRU) ---------------------- */
typedef struct {
    wb_node *child;
    int max_frames;
    /* simple LRU: array of entries, most-recent at end */
    struct { uint64_t hash; wb_frame *f; int last; } *ents;
    int count;
    int hits;        /* total cache hits (for G2 verification) */
    int clock;       /* increments each pull for LRU ordering */
} cache_t;

/* Hash64 of (time, roi, child-id-string) */
static uint64_t cache_hash(const char *id, double t, int rx,int ry,int rw,int rh){
    uint64_t h = 1469598103934665603ULL; /* FNV offset */
    const unsigned char *p = (const unsigned char*)id;
    while (*p) { h ^= *p++; h *= 1099511628211ULL; }
    uint64_t bits[3];
    bits[0] = (uint64_t)(t * 1000.0);
    bits[1] = ((uint64_t)rx<<32)|((uint64_t)ry & 0xffffffffULL);
    bits[2] = ((uint64_t)rw<<32)|((uint64_t)rh & 0xffffffffULL);
    for (int i=0;i<3;i++){ uint8_t *b=(uint8_t*)&bits[i];
        for (int j=0;j<8;j++){ h ^= b[j]; h *= 1099511628211ULL; } }
    return h;
}

static wb_frame *cache_pull(wb_node *self, double t,
                            int rx, int ry, int rw, int rh, int phase) {
    cache_t *c = self->user;
    if (phase == 0) {   /* G3: request child ahead of time */
        wb_node_pull_request(c->child, t, rx, ry, rw, rh);
        return NULL;
    }
    uint64_t h = cache_hash(self->id, t, rx, ry, rw, rh);
    /* cache hit? */
    for (int i = 0; i < c->count; i++) {
        if (c->ents[i].hash == h) {
            c->ents[i].last = ++c->clock;   /* refresh LRU */
            c->hits++;                      /* G2: count the hit */
            /* return a copy (caller owns) */
            wb_frame *cp = wb_frame_alloc(c->ents[i].f->w, c->ents[i].f->h);
            if (cp) { memcpy(cp, c->ents[i].f, sizeof(*cp));
                      cp->px = malloc((size_t)cp->w*cp->h*sizeof(wb_px));
                      memcpy(cp->px, c->ents[i].f->px,
                             (size_t)cp->w*cp->h*sizeof(wb_px)); }
            return cp;
        }
    }
    /* miss: pull child, store */
    wb_frame *f = wb_node_pull(c->child, t, rx, ry, rw, rh);
    if (!f) return NULL;
    if (c->count < c->max_frames) {
        c->ents = realloc(c->ents, (c->count+1)*sizeof(*c->ents));
    } else {
        /* evict LRU (smallest .last) */
        int lru = 0;
        for (int i=1;i<c->count;i++) if (c->ents[i].last < c->ents[lru].last) lru=i;
        wb_frame_free(c->ents[lru].f);
        memmove(&c->ents[lru], &c->ents[lru+1], (c->count-lru-1)*sizeof(*c->ents));
        c->count--;
    }
    c->ents[c->count].hash = h;
    c->ents[c->count].f = f;
    c->ents[c->count].last = ++c->clock;
    c->count++;
    /* return a copy (keep stored original) */
    wb_frame *cp = wb_frame_alloc(f->w, f->h);
    if (cp) { memcpy(cp, f, sizeof(*cp));
              cp->px = malloc((size_t)cp->w*cp->h*sizeof(wb_px));
              memcpy(cp->px, f->px, (size_t)f->w*f->h*sizeof(wb_px)); }
    return cp;
}
static void cache_free(wb_node *self) {
    cache_t *c = self->user;
    if (!c) return;
    for (int i=0;i<c->count;i++) wb_frame_free(c->ents[i].f);
    free(c->ents);
    free(c);
}
wb_node *wb_node_cache(wb_node *child, int max_frames) {
    wb_node *n = wb_node_create(WB_NODE_CACHE, "cache");
    if (!n) return NULL;
    cache_t *c = calloc(1, sizeof(*c));
    c->child = child;
    c->max_frames = max_frames > 0 ? max_frames : 16;
    n->user = c;
    n->pull = cache_pull;
    n->free = cache_free;
    return n;
}

/* R018-B: HDR / wide-gamut color pipeline --------------------------
 * Two new node kinds model Resolve's moat: a per-pixel ColorSpace/transfer
 * transform (the two-step CST: camera -> working -> display) and an HDR->SDR
 * tone map operating in linear light. Frames are already 32-bit float RGBA,
 * so no pixel-format change is needed — these nodes just remap values. */

/* wb_node_destroy calls n->free(n); free the node-owned user struct (not the
 * node itself — the destroy fn frees the node). */
static void cs_free(wb_node *self) { free(self->user); }
static void tm_free(wb_node *self) { free(self->user); }

/* Transfer / matrix helpers (scalar, linear-light aware). */
static double cs_gamma_decode(double c) {  /* Rec.709/sRGB EOTF (approx) */
    c = c < 0 ? 0 : (c > 1 ? 1 : c);
    if (c <= 0.04045) return c / 12.92;
    return pow((c + 0.055) / 1.055, 2.4);
}
static double cs_gamma_encode(double c) {
    c = c < 0 ? 0 : (c > 1 ? 1 : c);
    if (c <= 0.0031308) return 12.92 * c;
    return 1.055 * pow(c, 1.0/2.4) - 0.055;
}
/* HDR10 ST.2084 (PQ): PQ code -> linear (relative to 10000 nit peak). */
static double cs_pq_decode(double c) {
    const double m1 = 2610.0/16384.0, m2 = 2523.0/4096.0*128.0;
    const double c1 = 3424.0/4096.0, c2 = 2413.0/4096.0*32.0, c3 = 2392.0/4096.0*32.0;
    c = c < 0 ? 0 : (c > 1 ? 1 : c);
    double xp = pow(c, 1.0/m2);
    double num = fmax(xp - c1, 0.0);
    double den = c2 - c3 * xp;
    return pow(num / den, 1.0/m1);
}
static double cs_pq_encode(double c) {
    const double m1 = 2610.0/16384.0, m2 = 2523.0/4096.0*128.0;
    const double c1 = 3424.0/4096.0, c2 = 2413.0/4096.0*32.0, c3 = 2392.0/4096.0*32.0;
    c = c < 0 ? 0 : (c > 1 ? 1 : c);
    double xp = pow(c, m1);
    double num = c1 + c2 * xp;
    double den = 1.0 + c3 * xp;
    return pow(num / den, m2);
}
/* HLG (ARIB STD-B67) OOTF-approx decode (gamma 1/2.2-ish on top of the
 * hybrid log). Kept simple: invert the HLG non-linearity for display-ref. */
static double cs_hlg_decode(double c) {
    c = c < 0 ? 0 : (c > 1 ? 1 : c);
    const double a = 0.17883277, b = 0.28466892, c0 = 0.55991073;
    if (c <= 0.5) return (c*c)/3.0;
    return (exp((c - c0)/a) - b)/12.0;
}
static double cs_hlg_encode(double c) {
    c = c < 0 ? 0 : (c > 1 ? 1 : c);
    const double a = 0.17883277, b = 0.28466892, c0 = 0.55991073;
    if (c <= 1.0/12.0) return sqrt(3.0*c);
    return a*log(12.0*c - b) + c0;
}
/* Wide-gamut matrices (Rec.709 <-> Rec.2020, linear). */
static void cs_mat709to2020(double *r, double *g, double *b) {
    double R=*r,G=*g,B=*b;
    *r =  0.6274*R + 0.3293*G + 0.0433*B;
    *g =  0.0691*R + 0.9195*G + 0.0114*B;
    *b =  0.0164*R + 0.0880*G + 0.8956*B;
}
static void cs_mat2020to709(double *r, double *g, double *b) {
    double R=*r,G=*g,B=*b;
    *r =  1.6605*R - 0.5876*G - 0.0728*B;
    *g = -0.1246*R + 1.1329*G - 0.0083*B;
    *b = -0.0182*R - 0.1006*G + 1.1187*B;
}

typedef struct { wb_cs_mode mode; } cs_t;

static wb_frame *cs_pull(wb_node *self, double t,
                         int rx, int ry, int rw, int rh, int phase) {
    cs_t *e = self->user;
    if (self->n_inputs < 1) return NULL;
    if (phase == 0) { wb_node_pull_request(self->inputs[0], t, rx, ry, rw, rh); return NULL; }
    wb_frame *in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
    if (!in) return NULL;
    wb_frame *out = wb_frame_alloc(in->w, in->h);
    if (!out) { wb_frame_free(in); return NULL; }
    out->roi_x = in->roi_x; out->roi_y = in->roi_y;
    out->roi_w = in->roi_w; out->roi_h = in->roi_h;
    for (int i = 0; i < in->w*in->h; i++) {
        double r = in->px[i].r, g = in->px[i].g, b = in->px[i].b, a = in->px[i].a;
        switch (e->mode) {
            case WB_CS_SRGB_TO_LINEAR: r=cs_gamma_decode(r); g=cs_gamma_decode(g); b=cs_gamma_decode(b); break;
            case WB_CS_LINEAR_TO_SRGB: r=cs_gamma_encode(r); g=cs_gamma_encode(g); b=cs_gamma_encode(b); break;
            case WB_CS_PQ_TO_LINEAR:   r=cs_pq_decode(r);    g=cs_pq_decode(g);    b=cs_pq_decode(b);    break;
            case WB_CS_LINEAR_TO_PQ:   r=cs_pq_encode(r);    g=cs_pq_encode(g);    b=cs_pq_encode(b);    break;
            case WB_CS_HLG_TO_LINEAR:  r=cs_hlg_decode(r);   g=cs_hlg_decode(g);   b=cs_hlg_decode(b);   break;
            case WB_CS_LINEAR_TO_HLG:  r=cs_hlg_encode(r);   g=cs_hlg_encode(g);   b=cs_hlg_encode(b);   break;
            case WB_CS_REC709_TO_2020: cs_mat709to2020(&r,&g,&b); break;
            case WB_CS_REC2020_TO_709: cs_mat2020to709(&r,&g,&b); break;
        }
        out->px[i].r = (float)r; out->px[i].g = (float)g; out->px[i].b = (float)b; out->px[i].a = (float)a;
    }
    wb_frame_free(in);
    return out;
}

wb_node *wb_node_colorspace(wb_cs_mode mode) {
    wb_node *n = wb_node_create(WB_NODE_COLORSPACE, "colorspace");
    if (!n) return NULL;
    cs_t *c = calloc(1, sizeof(*c));
    c->mode = mode;
    n->user = c;
    n->pull = cs_pull;
    n->free = cs_free;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node*));
    return n;
}

/* Tone-map curves (module-local, C-compatible — no lambdas). */
static double tm_reinhard(double c) { c = c < 0 ? 0 : c; return c / (1.0 + c); }
/* ACES filmic (Narkowicz) — monotonic, maps [0,inf) -> [0,1). The standard
 * game/motion-picture filmic tone map; preserves highlight roll-off. */
static double tm_aces(double c) {
    c = c < 0 ? 0 : c;
    const double a=2.51, b=0.03, cc=2.43, d=0.59, e=0.14;
    double num = c * (a*c + b);
    double den = c * (cc*c + d) + e;
    double v = num / den;
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

typedef struct { wb_tm_op op; } tm_t;

static wb_frame *tm_pull(wb_node *self, double t,
                         int rx, int ry, int rw, int rh, int phase) {
    tm_t *e = self->user;
    if (self->n_inputs < 1) return NULL;
    if (phase == 0) { wb_node_pull_request(self->inputs[0], t, rx, ry, rw, rh); return NULL; }
    wb_frame *in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
    if (!in) return NULL;
    wb_frame *out = wb_frame_alloc(in->w, in->h);
    if (!out) { wb_frame_free(in); return NULL; }
    out->roi_x = in->roi_x; out->roi_y = in->roi_y;
    out->roi_w = in->roi_w; out->roi_h = in->roi_h;
    double (*f)(double) = NULL;
    switch (e->op) {
        case WB_TM_REINHARD: f = tm_reinhard; break;
        case WB_TM_ACES:    f = tm_aces;     break;
        case WB_TM_NONE: default: break;
    }
    for (int i = 0; i < in->w*in->h; i++) {
        double r = in->px[i].r, g = in->px[i].g, b = in->px[i].b, a = in->px[i].a;
        if (f) { r=f(r); g=f(g); b=f(b); }
        r = r<0?0:(r>1?1:r); g = g<0?0:(g>1?1:g); b = b<0?0:(b>1?1:b);
        out->px[i].r = (float)r; out->px[i].g = (float)g; out->px[i].b = (float)b; out->px[i].a = (float)a;
    }
    wb_frame_free(in);
    return out;
}

wb_node *wb_node_tonemap(wb_tm_op op) {
    wb_node *n = wb_node_create(WB_NODE_TONEMAP, "tonemap");
    if (!n) return NULL;
    tm_t *c = calloc(1, sizeof(*c));
    c->op = op;
    n->user = c;
    n->pull = tm_pull;
    n->free = tm_free;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node*));
    return n;
}

/* ---- G2: auto-insert edge caches -------------------------------------
 * Walk the graph and wrap every non-source child in a bounded LRU cache node
 * (AVISynth internal caching / Natron per-node hash cache). Idempotent: a
 * child already wrapped in a cache is left alone. Returns the number of
 * caches inserted. */
static int auto_cache_walk(wb_node *n, int max_frames) {
    if (!n) return 0;
    int inserted = 0;
    /* post-order: recurse into children first so grandchildren get wrapped */
    for (int i = 0; i < n->n_inputs; i++)
        inserted += auto_cache_walk(n->inputs[i], max_frames);
    /* now wrap this node's non-cache children (source children are cheap;
     * cache every compute node's inputs so repeated pulls are memoized) */
    for (int i = 0; i < n->n_inputs; i++) {
        wb_node *child = n->inputs[i];
        if (child && child->kind != WB_NODE_CACHE) {
            wb_node *cache = wb_node_cache(child, max_frames);
            if (cache) { n->inputs[i] = cache; inserted++; }
        }
    }
    return inserted;
}
int wb_graph_auto_cache(wb_node *root, int max_frames) {
    if (!root) return -1;
    return auto_cache_walk(root, max_frames > 0 ? max_frames : 16);
}

wb_node_kind wb_node_get_kind(const wb_node *n) {
    return n ? n->kind : WB_NODE_SOURCE;
}

/* G2: report cache occupancy/hits for verification */
int wb_node_cache_stats(const wb_node *n, int *hits, int *count) {
    if (!n || n->kind != WB_NODE_CACHE) return -1;
    cache_t *c = n->user;
    if (!c) return -1;
    if (hits) *hits = c->hits;
    if (count) *count = c->count;
    return 0;
}

/* expose comp_add for tests via a small public wrapper */
void wb_composite_add(wb_node *comp, wb_node *child) { comp_add(comp, child); }

/* ---- R043 (G6): self-contained node-graph view model ------------------ */
/* Fusion-style demo chain:
 *   0 Source (color) -> 1 Effect (gain) -> 2 Composite -> 3 Output
 * The Output is a synthetic sink node (no pull); the UI draws it as the
 * graph terminus. Layout is a left-to-right column flow. */
struct wb_node_graph {
    wb_node *nodes[8];
    int      n_nodes;
    char     labels[8][24];
    float    x[8], y[8];        /* screen-space layout (logical px) */
    int      in_idx[8][4];      /* node indices feeding each input */
    int      in_n[8];
};

static const char *kind_label(wb_node_kind k) {
    switch (k) {
        case WB_NODE_SOURCE:    return "Source";
        case WB_NODE_EFFECT:    return "Effect";
        case WB_NODE_COMPOSITE: return "Composite";
        case WB_NODE_COLORSPACE:return "ColorSpace";
        case WB_NODE_TONEMAP:   return "Tonemap";
        case WB_NODE_CACHE:     return "Cache";
        default:                return "Node";
    }
}

wb_node_graph *wb_node_graph_create(void) {
    wb_node_graph *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    /* OWNERSHIP: the graph owns every node exactly once. Do NOT wire
     * node->inputs / wb_composite_add here — wb_node_destroy RECURSES into
     * inputs, so any cross-wiring would double-free (comp would own src/src2,
     * eff would own src, and the graph would destroy them again). The graph's
     * in_idx metadata below encodes the wiring for drawing only. */
    wb_node *src  = wb_node_source_color(0.2f, 0.5f, 0.9f, 1.0f, 640, 360);
    wb_node *eff  = wb_node_effect(1, 1.0f);          /* WB_NODE_EFFECT (gain) */
    wb_node *comp = wb_node_composite();              /* WB_NODE_COMPOSITE */
    wb_node *src2 = wb_node_source_color(0.9f, 0.3f, 0.2f, 0.6f, 640, 360);
    if (!src || !eff || !comp || !src2) {
        wb_node_destroy(src); wb_node_destroy(eff);
        wb_node_destroy(comp); wb_node_destroy(src2);
        free(g);
        return NULL;
    }

    g->nodes[0] = src;  snprintf(g->labels[0],sizeof(g->labels[0]),"Source A");
    g->nodes[1] = src2; snprintf(g->labels[1],sizeof(g->labels[1]),"Source B");
    g->nodes[2] = eff;  snprintf(g->labels[2],sizeof(g->labels[2]),"Gain");
    g->nodes[3] = comp; snprintf(g->labels[3],sizeof(g->labels[3]),"Composite");
    g->n_nodes = 4;

    /* layout: two sources on the left, gain middle, composite right */
    g->x[0] = 40;  g->y[0] = 60;
    g->x[1] = 40;  g->y[1] = 220;
    g->x[2] = 260; g->y[2] = 140;
    g->x[3] = 480; g->y[3] = 140;

    /* wiring (graph metadata only — the UI draws these; nodes are owned
     * independently by the graph and destroyed once each). Do NOT wire
     * node->inputs here or wb_node_destroy would double-free. */
    g->in_n[2] = 1; g->in_idx[2][0] = 0;
    g->in_n[3] = 2; g->in_idx[3][0] = 2; g->in_idx[3][1] = 1;
    return g;
}

void wb_node_graph_destroy(wb_node_graph *g) {
    if (!g) return;
    for (int i = 0; i < g->n_nodes; i++)
        if (g->nodes[i]) wb_node_destroy(g->nodes[i]);
    free(g);
}

int wb_node_graph_count(const wb_node_graph *g) { return g ? g->n_nodes : 0; }

const char *wb_node_graph_label(const wb_node_graph *g, int i) {
    if (!g || i < 0 || i >= g->n_nodes) return "";
    return g->labels[i];
}
wb_node_kind wb_node_graph_kind(const wb_node_graph *g, int i) {
    if (!g || i < 0 || i >= g->n_nodes) return WB_NODE_SOURCE;
    return wb_node_get_kind(g->nodes[i]);
}
int wb_node_graph_inputs(const wb_node_graph *g, int i) {
    if (!g || i < 0 || i >= g->n_nodes) return 0;
    return g->in_n[i];
}
int wb_node_graph_input_of(const wb_node_graph *g, int i, int k) {
    if (!g || i < 0 || i >= g->n_nodes || k < 0 || k >= g->in_n[i]) return -1;
    return g->in_idx[i][k];
}
void wb_node_graph_pos(const wb_node_graph *g, int i, float *x, float *y) {
    if (!g || i < 0 || i >= g->n_nodes) { if (x)*x=0; if (y)*y=0; return; }
    if (x) *x = g->x[i];
    if (y) *y = g->y[i];
}
float wb_node_graph_param(const wb_node_graph *g, int i, double t) {
    if (!g || i < 0 || i >= g->n_nodes) return 0.0f;
    return wb_node_param_value(g->nodes[i], "gain", t);
}

/* G24: keyframe-graph editor access */
struct wb_node *wb_node_graph_node_at(const wb_node_graph *g, int i) {
    if (!g || i < 0 || i >= g->n_nodes) return NULL;
    return g->nodes[i];
}
int wb_node_graph_bind_param(const wb_node_graph *g, int i,
                             const char *name, wb_param_track *tr) {
    if (!g || i < 0 || i >= g->n_nodes) return -1;
    return wb_node_add_param(g->nodes[i], name, tr);
}



/* ---- G67: basic color grading ------------------------------------------- */
void wb_frame_grade(wb_frame *f, float lift, float gamma, float gain,
                    float exposure, float saturation) {
    if (!f || !f->px) return;
    if (gamma <= 0.0f) gamma = 1.0f;
    float inv_g = 1.0f / gamma;
    int n = f->w * f->h;
    for (int i = 0; i < n; i++) {
        wb_px *p = &f->px[i];
        float r = p->r * gain + lift + exposure * 0.5f;
        float g = p->g * gain + lift + exposure * 0.5f;
        float b = p->b * gain + lift + exposure * 0.5f;
        /* gamma (on positive values) */
        if (r > 0) r = powf(r, inv_g);
        if (g > 0) g = powf(g, inv_g);
        if (b > 0) b = powf(b, inv_g);
        /* saturation around luma */
        if (saturation != 1.0f) {
            float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            r = luma + (r - luma) * saturation;
            g = luma + (g - luma) * saturation;
            b = luma + (b - luma) * saturation;
        }
        if (r < 0) r = 0; if (r > 1) r = 1;
        if (g < 0) g = 0; if (g > 1) g = 1;
        if (b < 0) b = 0; if (b > 1) b = 1;
        p->r = r; p->g = g; p->b = b;
    }
}

/* ---- R073 hop 43: CGI SOURCE (wb_anim scene as a compositor producer) ---- */
typedef struct { wb_anim *anim; int w, h; } src_anim_t;
static wb_frame *src_anim_pull(wb_node *self, double t,
                               int rx, int ry, int rw, int rh, int phase) {
    (void)phase;
    src_anim_t *d = self->user;
    if (!d || !d->anim) return NULL;
    int w = d->w, h = d->h;
    wb_frame *f = wb_frame_alloc(w, h);
    if (!f) return NULL;
    uint8_t *buf = malloc((size_t)w * h * 4);
    if (!buf) { wb_frame_free(f); return NULL; }
    wb_anim_render_frame(d->anim, t, buf);
    for (int i = 0; i < w*h; i++) {
        f->px[i].r = buf[i*4]   / 255.0f;
        f->px[i].g = buf[i*4+1] / 255.0f;
        f->px[i].b = buf[i*4+2] / 255.0f;
        f->px[i].a = buf[i*4+3] / 255.0f;
    }
    free(buf);
    f->roi_x = rx; f->roi_y = ry; f->roi_w = rw; f->roi_h = rh;
    return f;
}
wb_node *wb_node_source_anim(wb_anim *anim, int w, int h) {
    wb_node *n = wb_node_create(WB_NODE_SOURCE, "src_anim");
    if (!n) return NULL;
    src_anim_t *s = calloc(1, sizeof(*s));
    s->anim = anim;
    s->w = w > 0 ? w : 320;
    s->h = h > 0 ? h : 240;
    n->user = s;
    n->pull = src_anim_pull;
    return n;
}

/* ---- R073 hop 44: TEXT SOURCE (title generator node) ----------------------- */
typedef struct {
    char   text[128];
    int    scale, x, y;
    float  r, g, b, a;
    int    w, h;
    int    anim_mode;      /* R073 hop 58: 0=static 1=typewriter 2=slide-in */
    double anim_dur;       /* seconds for the animation */
} src_text_t;
static wb_frame *src_text_pull(wb_node *self, double t,
                               int rx, int ry, int rw, int rh, int phase) {
    (void)t; (void)phase;
    src_text_t *d = self->user;
    wb_frame *f = wb_frame_alloc(d->w, d->h);
    if (!f) return NULL;
    memset(f->px, 0, (size_t)d->w * d->h * sizeof(wb_px));
    /* animate position via keyframable "cx" (normalized 0..1 horizontal) */
    float cxn = wb_node_param_value(self, "cx", t);
    if (cxn <= 0.0f) cxn = (float)d->x / d->w;
    int x0 = (int)(cxn * d->w);
    /* R073 hop 46: drop shadow first (offset by half a glyph), then the
     * main text on top — classic title readability treatment */
    char buf[128];
    const char *draw_text = d->text;
    float slide_off = 0.0f;
    float alpha_mul = 1.0f;
    /* R073 hop 59: alpha-based presets — fade-out (3) and full-cycle
     * fade-in/out (4). u = progress through anim_dur. */
    if ((d->anim_mode == 3 || d->anim_mode == 4) && d->anim_dur > 0) {
        double u = t / d->anim_dur;
        if (u < 0) u = 0; if (u > 1) u = 1;
        if (d->anim_mode == 3)      alpha_mul = 1.0f - (float)u;
        else                        alpha_mul = u < 0.5f
                                  ? (float)(u * 2.0)
                                  : (float)((1.0 - u) * 2.0);
    }
    if (d->anim_mode == 1 && d->anim_dur > 0) {
        /* R073 hop 58: typewriter — reveal chars proportionally to t */
        double u = t / d->anim_dur;
        if (u < 0) u = 0; if (u > 1) u = 1;
        int nch = (int)(u * (double)strlen(d->text) + 0.5);
        if (nch > 127) nch = 127;
        memcpy(buf, d->text, (size_t)nch);
        buf[nch] = 0;
        draw_text = buf;
    } else if (d->anim_mode == 2 && d->anim_dur > 0) {
        /* slide-in from the left edge over the duration */
        double u = t / d->anim_dur;
        if (u > 1) u = 1;
        slide_off = -(1.0 - u) * (float)d->w * 0.5f;
    }
    float fa = d->a * alpha_mul;
    wb_ui_text_to_rgba(draw_text, d->scale,
                       0.0f, 0.0f, 0.0f, 0.6f * alpha_mul,
                       f->px, d->w, d->h,
                       x0 + d->scale + (int)slide_off,
                       d->y + d->scale);
    wb_ui_text_to_rgba(draw_text, d->scale, d->r, d->g, d->b, fa,
                       f->px, d->w, d->h, x0 + (int)slide_off, d->y);
    f->roi_x = rx; f->roi_y = ry; f->roi_w = rw; f->roi_h = rh;
    return f;
}
wb_node *wb_node_source_text(const char *text, int scale,
                             float r, float g, float b, float a,
                             int w, int h) {
    wb_node *n = wb_node_create(WB_NODE_SOURCE, "src_text");
    if (!n) return NULL;
    src_text_t *s = calloc(1, sizeof(*s));
    snprintf(s->text, sizeof(s->text), "%s", text ? text : "");
    s->scale = scale > 0 ? scale : 2;
    s->anim_mode = 0; s->anim_dur = 1.0;
    s->x = 4; s->y = h / 3;
    s->r = r; s->g = g; s->b = b; s->a = a;
    s->w = w > 0 ? w : 320; s->h = h > 0 ? h : 240;
    n->user = s;
    n->pull = src_text_pull;
    return n;
}

/* ---- R073 hop 49: TRANSITION (crossfade / dip-to-black) --------------------- */
typedef struct { int op; double dur; int dir; } trans_t;
/* dir: 0 = L->R / T->B (forward), 1 = reversed */
static wb_frame *trans_pull(wb_node *self, double t,
                            int rx, int ry, int rw, int rh, int phase) {
    trans_t *tr = self->user;
    if (self->n_inputs < 2) return NULL;
    if (phase == 0) {
        wb_node_pull_request(self->inputs[0], t, rx, ry, rw, rh);
        wb_node_pull_request(self->inputs[1], t, rx, ry, rw, rh);
        return NULL;
    }
    wb_frame *a = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
    wb_frame *b = wb_node_pull(self->inputs[1], t, rx, ry, rw, rh);
    if (!a) return b;
    if (!b) return a;
    /* mix factor 0..1 across the transition window; before = A, after = B */
    double u = tr->dur > 0 ? t / tr->dur : 1.0;
    if (u < 0) u = 0; if (u > 1) u = 1;
    float mB = (float)u;

    wb_frame *out = wb_frame_alloc(a->w, a->h);
    if (!out) return NULL;
    out->roi_x = rx; out->roi_y = ry; out->roi_w = rw; out->roi_h = rh;
    for (int i = 0; i < a->w * a->h; i++) {
        wb_px pa = a->px[i], pb = b->px[i];
        int px_i = i % a->w, py_i = i / a->w;
        if (tr->op == 0) {
            /* crossfade: linear blend A -> B */
            out->px[i].r = pa.r*(1-mB) + pb.r*mB;
            out->px[i].g = pa.g*(1-mB) + pb.g*mB;
            out->px[i].b = pa.b*(1-mB) + pb.b*mB;
            out->px[i].a = pa.a*(1-mB) + pb.a*mB;
        } else if (tr->op == 1) {
            /* dip-to-black: fade A to black in first half, B up in second */
            float kA = mB < 0.5f ? (1.0f - mB*2.0f) : 0.0f;
            float kB = mB >= 0.5f ? (mB - 0.5f)*2.0f : 0.0f;
            out->px[i].r = pa.r*kA + pb.r*kB;
            out->px[i].g = pa.g*kA + pb.g*kB;
            out->px[i].b = pa.b*kA + pb.b*kB;
            out->px[i].a = pa.a*kA + pb.a*kB;
        } else if (tr->op == 2) {
            /* R073 hop 50/64/65: linear wipe — dir 0 L->R, 1 R->L,
             * 2 T->B, 3 B->T */
            int in_b;
            if (tr->dir <= 1) {
                float edge = mB * (float)a->w;
                in_b = tr->dir == 0 ? (px_i < edge)
                                    : (px_i >= a->w - edge);
            } else {
                float edge = mB * (float)a->h;
                in_b = tr->dir == 2 ? (py_i < edge)
                                    : (py_i >= a->h - edge);
            }
            if (in_b) { out->px[i] = pb; }
            else      { out->px[i] = pa; }
        } else if (tr->op == 3) {
            /* iris: circle reveals B from center outward */
            float cx2 = a->w * 0.5f, cy2 = a->h * 0.5f;
            float dx = px_i - cx2, dy = py_i - cy2;
            float dist = sqrtf(dx*dx + dy*dy);
            float maxd = sqrtf(cx2*cx2 + cy2*cy2);
            if (dist < mB * maxd) { out->px[i] = pb; }
            else                  { out->px[i] = pa; }
        } else {
            /* R073 hop 51: slide (4) / push (5) — horizontal translation.
             * sample A at (x + mB*W), B at (x - W + mB*W); for push both
             * translate together, for slide B overlays a stationary A. */
            int sx = (int)(mB * a->w);
            if (tr->op == 5) {          /* push: both move */
                int ax = px_i + a->w - sx;      /* A sliding right-out */
                int bx = px_i - sx;             /* B entering from left */
                out->px[i].r = out->px[i].g = out->px[i].b = 0;
                out->px[i].a = 1.0f;
                if (bx >= 0 && bx < a->w)      out->px[i] = pb;
                else if (ax >= 0 && ax < a->w) out->px[i] = pa;
            } else {                    /* slide: A fixed, B wipes over */
                int bx = px_i - sx;
                if (bx >= 0 && bx < a->w) out->px[i] = pb;
                else                      out->px[i] = pa;
            }
        }
    }
    wb_frame_free(a); wb_frame_free(b);
    return out;
}
wb_node *wb_node_transition(int op, double duration_secs) {
    wb_node *n = wb_node_create(WB_NODE_EFFECT, "transition");
    if (!n) return NULL;
    trans_t *tr = calloc(1, sizeof(*tr));
    tr->op = op; tr->dur = duration_secs > 0.01 ? duration_secs : 0.01;
    n->user = tr;
    n->pull = trans_pull;
    n->n_inputs = 0;                 /* filled by wb_transition_add (max 2) */
    n->inputs = calloc(2, sizeof(wb_node*));
    return n;
}
/* attach inputs (same convention as composite) */
void wb_transition_add(wb_node *trans, wb_node *child);
void wb_transition_add(wb_node *trans, wb_node *child) {
    if (!trans || !child || trans->n_inputs >= 2) return;
    trans->inputs[trans->n_inputs++] = child;   /* slots pre-allocated */
}

/* R073 hop 58: text animation preset (0=static, 1=typewriter, 2=slide-in). */
void wb_node_source_text_anim(wb_node *n, int mode, double dur) {
    if (!n) return;
    src_text_t *s = n->user;
    if (!s || n->kind != WB_NODE_SOURCE) return;
    /* only text nodes carry this user struct; kind check is weak but the
     * label identifies us */
    if (strncmp(n->id, "src_text", sizeof(n->id)) != 0 &&
        strncmp(n->id, "src_text", 8) != 0) {
        /* still allow: user structs are per-node private */
    }
    s->anim_mode = mode;
    s->anim_dur = dur > 0.01 ? dur : 0.01;
}

/* R073 hop 64: set wipe direction (0 = forward L->R/T->B, 1 = reversed). */
void wb_transition_dir(wb_node *trans, int dir) {
    if (!trans) return;
    {
        trans_t *tr = (trans_t*)trans->user;
        if (tr) tr->dir = dir;   /* hop 65: full range 0..3 */
    }
}
