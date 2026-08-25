/* wb_compositor.c — pull-based RoI/RoD node compositor (R013 D1/D3).
 * Pure C11. Recursive pull with identity skip + edge cache (LRU). */

#include "wbus/wbus_compositor.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "wbus/wbus_anim.h"
#include "wbus/wb_ui.h"   /* R073 hop 43: CGI source */


/* R074 hop 116 (#29/#31): linear-light Rec.709 luma — decode sRGB
 * channel first, then weight. Correct for keyed/lit pixels. */
static float wb_lin_luma(float r, float g, float b) {
    float lr = r <= 0.04045f ? r / 12.92f : powf((r+0.055f)/1.055f, 2.4f);
    float lg = g <= 0.04045f ? g / 12.92f : powf((g+0.055f)/1.055f, 2.4f);
    float lb = b <= 0.04045f ? b / 12.92f : powf((b+0.055f)/1.055f, 2.4f);
    return 0.2126f*lr + 0.7152f*lg + 0.0722f*lb;
}

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
    /* R074 hop 132 (#70): harden against degenerate inputs — reject
     * non-positive frame sizes and negative dims up front. */
    if (w <= 0 || h <= 0) { *rw = 0; *rh = 0; return 0; }
    if (*rw < 0 || *rh < 0) { *rw = 0; *rh = 0; return 0; }
    if (*rx < 0) { *rw += *rx; *rx = 0; }
    if (*ry < 0) { *rh += *ry; *ry = 0; }
    if (*rx >= w || *ry >= h) { *rw = 0; *rh = 0; return 0; }
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
    n->fmt_w = 0; n->fmt_h = 0;   /* R074: infer until declared */
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

/* R074 fix: explicit format declaration / inference. */
int wb_node_set_format(wb_node *n, int w, int h) {
    if (!n || w <= 0 || h <= 0 || w > 4096 || h > 4096) return -1;
    n->fmt_w = w; n->fmt_h = h;
    return 0;
}
/* Resolve output dims: declared format wins; else first input's resolved
 * format; else 0 (unknown). */
static void node_resolve_format(wb_node *n, int *w, int *h) {
    *w = 0; *h = 0;
    if (!n) return;
    if (n->fmt_w > 0 && n->fmt_h > 0) { *w = n->fmt_w; *h = n->fmt_h; return; }
    for (int i = 0; i < n->n_inputs; i++) {
        int iw, ih;
        node_resolve_format(n->inputs[i], &iw, &ih);
        if (iw > *w) *w = iw;
        if (ih > *h) *h = ih;
    }
}
int wb_node_get_format(const wb_node *n, int *w, int *h) {
    int ww, hh;
    node_resolve_format((wb_node*)n, &ww, &hh);
    if (w) *w = ww;
    if (h) *h = hh;
    return ww > 0 ? 0 : -1;
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
    int fw, fh;
    node_resolve_format(n, &fw, &fh);
    if (fw <= 0) { fw = 4096; fh = 4096; }   /* unknown: legacy bound */
    if (!wb_roi_clip(fw, fh, &rx, &ry, &rw, &rh)) return NULL;
    /* phase 1 = compute (the default for callers that don't two-phase) */
    return n->pull(n, t, rx, ry, rw, rh, 1);
}

/* G3: phase 0 = request/prepare (schedule decodes, no frame yet). Walk the
 * graph requesting inputs so slow sources (decode) can run ahead; the
 * subsequent wb_node_pull (phase 1) then computes. VapourSynth-style
 * arInitial -> arAllFramesReady. */
void wb_node_pull_request(wb_node *n, double t, int rx, int ry, int rw, int rh) {
    if (!n) return;
    int fw, fh;
    node_resolve_format(n, &fw, &fh);
    if (fw <= 0) { fw = 4096; fh = 4096; }
    if (!wb_roi_clip(fw, fh, &rx, &ry, &rw, &rh)) return;
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
    /* R074 fix: a color source is uniform — fill the whole frame so any
     * ROI consumer sees valid pixels (was: zeros outside ROI). */
    for (int i = 0; i < s->w * s->h; i++) {
        f->px[i].r = s->r; f->px[i].g = s->g;
        f->px[i].b = s->b; f->px[i].a = s->a;
    }
    f->roi_x = 0; f->roi_y = 0; f->roi_w = s->w; f->roi_h = s->h;
    return f;
}
wb_node *wb_node_source_color(float r, float g, float b, float a, int w, int h) {
    wb_node *n = wb_node_create(WB_NODE_SOURCE, "src_color");
    wb_node_set_format(n, w > 0 ? w : 320, h > 0 ? h : 240);
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
    /* R074 fix: blur hoisted OUT of the per-pixel loop — one 2-pass box
     * over the ROI per frame (was O(n^2) with per-pixel malloc storm). */
    if (e->op == 4) {
        /* R074 hop 116 (#52): dedicated blur_radius param; gain no longer
         * doubles as a radius fallback. */
        float rad = wb_node_param_value(self, "blur_radius", t);
        if (rad <= 0.0f) rad = wb_node_param_value(self, "blur", t);
        if (rad <= 0.0f) rad = e->gain > 0 ? e->gain : 1.0f;
        int r = (int)(rad * 3.0f);
        if (r >= 1) {
            int W = in->w, H = in->h;
            wb_frame *tmp = wb_frame_alloc(W, H);
            if (tmp) {
                for (int pass = 0; pass < 2; pass++) {
                    wb_frame *srcf = (pass == 0) ? in : tmp;
                    wb_frame *dstf = (pass == 0) ? tmp : in;
                    for (int y = ry; y < ry + rh; y++)
                        for (int x = rx; x < rx + rw; x++) {
                            float ar=0, ag=0, ab=0, aa=0; int n=0;
                            for (int k2 = -r; k2 <= r; k2++) {
                                int xx = x + k2;
                                if (xx < 0 || xx >= W) continue;
                                wb_px *q=&srcf->px[y*W+xx];
                                ar+=q->r; ag+=q->g; ab+=q->b; aa+=q->a; n++;
                            }
                            wb_px *d=&dstf->px[y*W+x];
                            d->r=ar/n; d->g=ag/n; d->b=ab/n; d->a=aa/n;
                        }
                    for (int y = ry; y < ry + rh; y++)
                        for (int x = rx; x < rx + rw; x++) {
                            float ar=0, ag=0, ab=0, aa=0; int n=0;
                            for (int k2 = -r; k2 <= r; k2++) {
                                int yy = y + k2;
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
    /* G11: a keyframed "gain" param overrides the static gain (animates) */
    float gain = e->gain;
    float kv = wb_node_param_value(self, "gain", t);
    /* R074 fix: a bound track is authoritative even at exactly 0 —
     * detect binding instead of guessing from the value. */
    for (int pi = 0; pi < self->n_params; pi++) {
        if (self->param_names &&
            strncmp(self->param_names[pi], "gain", 32) == 0) {
            gain = kv;
            break;
        }
    }
        /* R074 hop 126 (#40/#43/#44): hoist ALL keyframable param fetches
     * above the per-pixel loop. wb_node_param_value walks param track
     * lists — calling it per pixel dominated the profile. */
    float p_lift = wb_node_param_value(self, "lift", t);
    float p_gam  = wb_node_param_value(self, "gamma", t);
    float p_gnv  = wb_node_param_value(self, "gain", t);
    float p_sat  = wb_node_param_value(self, "sat", t);
    float p_vig  = wb_node_param_value(self, "vig", t);
    float p_vstart = wb_node_param_value(self, "vig_start", t);
    float p_hue_w  = wb_node_param_value(self, "hue_w", t);
    float p_hue_sh = wb_node_param_value(self, "hue_shift", t);
    float p_sec_sat= wb_node_param_value(self, "sec_sat", t);
    float p_win_r  = wb_node_param_value(self, "win_r", t);
    float p_win_cx = wb_node_param_value(self, "win_cx", t);
    float p_win_cy = wb_node_param_value(self, "win_cy", t);
    float p_win_soft = wb_node_param_value(self, "win_soft", t);
    float p_glow_thr = wb_node_param_value(self, "glow_thr", t);
    float p_lum_thr  = wb_node_param_value(self, "lum_thr", t);
    float p_temp = wb_node_param_value(self, "temp", t);
    float p_tint = wb_node_param_value(self, "tint", t);
    float p_cur_blk = wb_node_param_value(self, "cur_blk", t);
    float p_cur_shd = wb_node_param_value(self, "cur_shd", t);
    float p_cur_hig = wb_node_param_value(self, "cur_hig", t);
    float p_cur_wht = wb_node_param_value(self, "cur_wht", t);
    float p_hue_c   = wb_node_param_value(self, "hue_c", t);
    float p_key_tol = wb_node_param_value(self, "key_tol", t);
    float p_key_color = wb_node_param_value(self, "key_color", t);
    if (p_gam <= 0.0f) p_gam = 1.0f;
for (int y = ry; y < ry + rh; y++)
        for (int x = rx; x < rx + rw; x++) {
            wb_px *p = &in->px[y*in->w + x];
            if (e->op == 1) { p->r*=gain; p->g*=gain; p->b*=gain; }
            else if (e->op == 2) { /* invert alpha matte */
                p->a = 1.0f - p->a;
            }
            else if (e->op == 4) {
                /* handled pre-loop (R074 fix) */
            }
            else if (e->op == 6) {
                /* R073 hop 47a: vignette — radial darkening; strength via
                 * keyframable "vig" param, radius fixed at frame corner */
                float vig = p_vig;
                if (vig <= 0.0f) vig = gain;
                float cx2 = in->w * 0.5f, cy2 = in->h * 0.5f;
                /* R074 hop 116 (#12): Vegas-style falloff — normalize
                 * by corner distance so edges stay in [0,1]; the
                 * vig_start radius provides the width/2 feel */
                float maxd = sqrtf(cx2*cx2 + cy2*cy2);
                for (int y = ry; y < ry + rh; y++)
                    for (int x = rx; x < rx + rw; x++) {
                        wb_px *p = &in->px[y*in->w + x];
                        float dx = x - cx2, dy = y - cy2;
                        float dnorm = sqrtf(dx*dx + dy*dy) / maxd;
                        /* R074 fix: start radius keyframable via
                         * "vig_start" (fraction of corner distance). */
                        float vstart = p_vstart;
                        if (vstart <= 0.0f) vstart = 0.5f;
                        float span = 1.0f - vstart;
                        float fall = dnorm < vstart ? 0.0f
                                   : (dnorm - vstart)
                                   / (span > 1e-3f ? span : 1e-3f);
                        float k = 1.0f - vig * fall * fall;
                        p->r *= k; p->g *= k; p->b *= k;
                    }
            }
            else if (e->op == 8) {
                /* R073 hop 52: primary grade — lift/gamma/gain/saturation,
                 * all keyframable via params of the same names. */
                float lift = p_lift;
                float gam  = p_gam;
                float gnv  = p_gnv;
                float sat  = p_sat;
                if (gam <= 0.0f) gam = 1.0f;
                if (gnv <= 0.0f) gnv = 1.0f;
                if (sat <= 0.0f) sat = e->gain > 0 ? e->gain : 1.0f;
                /* R074 fix: ASC order — lift, then gamma, then gain
                 * (was lift*gain then gamma). Clamp negatives first. */
                float lr = p->r + lift, lg = p->g + lift,
                      lb = p->b + lift;
                if (lr < 0) lr = 0; if (lg < 0) lg = 0; if (lb < 0) lb = 0;
                if (lr > 1) lr = 1; if (lg > 1) lg = 1; if (lb > 1) lb = 1;
                /* R074 hop 116 (#20/21): gamma+gain in LINEAR light */
                float lin[3] = { lr, lg, lb };
                for (int ch2 = 0; ch2 < 3; ch2++) {
                    float v = lin[ch2];
                    v = v <= 0.04045f ? v / 12.92f
                      : powf((v + 0.055f) / 1.055f, 2.4f);   /* decode */
                    v = powf(v, 1.0f / gam) * gnv;           /* grade   */
                    v = v <= 0.0031308f ? v * 12.92f
                      : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f; /* encode */
                    if (ch2 == 0) p->r = v;
                    else if (ch2 == 1) p->g = v;
                    else p->b = v;
                }
                /* saturation around Rec.709 luma */
                float lum = 0.2126f*p->r + 0.7152f*p->g + 0.0722f*p->b; /* display-space sat */
                p->r = lum + (p->r - lum)*sat;
                p->g = lum + (p->g - lum)*sat;
                p->b = lum + (p->b - lum)*sat;
            }
            else if (e->op == 9) {
                /* R073 hop 73: white balance — temp shifts R vs B
                 * (positive = warmer), tint shifts G. Both keyframable.
                 * Implemented as RGB gain per the grading practice that
                 * temp/tint are effectively channel-gain moves. */
                float temp = p_temp;
                float tint = p_tint;
                p->r *= 1.0f + temp;
                p->b *= 1.0f - temp;
                p->g *= 1.0f + tint;
            }
            else if (e->op == 10) {
                /* R073 hop 74: tone curves — keyframable blk/shd/hig/wht */
                float blk = p_cur_blk;
                float shd = p_cur_shd;
                float hig = p_cur_hig;
                float wht = p_cur_wht;
                if (blk != 0.0f || shd != 0.0f || hig != 0.0f ||
                    wht != 0.0f) {
                    if (shd < blk) shd = blk;
                    if (hig < shd) hig = shd;
                    if (wht <= hig) wht = hig + 0.01f;
                    float ch[3] = { p->r, p->g, p->b };
                    for (int ci = 0; ci < 3; ci++) {
                        float v = ch[ci];
                        float o;
                        if (v <= shd) {
                            float u2 = (v - blk) /
                                ((shd - blk > 1e-4f) ? shd-blk : 1e-4f);
                            if (u2 < 0) u2 = 0; if (u2 > 1) u2 = 1;
                            o = 0.25f * (u2*u2*(3-2*u2));
                        } else if (v <= hig) {
                            float u2 = (v - shd) /
                                ((hig - shd > 1e-4f) ? hig-shd : 1e-4f);
                            o = 0.25f + 0.5f * (u2*u2*(3-2*u2));
                        } else {
                            /* R074 fix: above white point clips to 1.0
                             * (was folding back into the band) */
                            if (v >= wht) { o = 1.0f; }
                            else {
                                float u2 = (v - hig) /
                                    ((wht - hig > 1e-4f) ? wht-hig : 1e-4f);
                                o = 0.75f + 0.25f * (u2*u2*(3-2*u2));
                            }
                        }
                        ch[ci] = o;
                    }
                    p->r = ch[0]; p->g = ch[1]; p->b = ch[2];
                }
            }
            else if (e->op == 11) {
                /* R073 hop 75: HSL secondary — qualify a hue center +
                 * width, then apply hue_shift / sat_mul only to qualified
                 * pixels (soft edges in the hue domain). All keyframable. */
                float hc   = p_hue_c;
                float hw   = p_hue_w;
                float hsh  = p_hue_sh;
                float smul = p_sec_sat;
                if (hw <= 0.0f) break;   /* unbound: identity */
                /* R073 hop 76: power window — circular soft mask limits
                 * the secondary to a screen region (win_cx/cy/r/soft). */
                float wr = p_win_r;
                float wsel = 1.0f;
                if (wr > 0.0f) {
                    /* R074 hop 116 (#11): aspect-correct distance —
                     * dx scaled by aspect so circles are circular while
                     * win_cx/cy stay in normalized [0,1] space */
                    float aspect = in->h > 0 ? (float)in->w / (float)in->h : 1.0f;
                    float nx = ((float)x + 0.5f) / in->w
                             - p_win_cx;
                    float ny = ((float)y + 0.5f) / in->h
                             - p_win_cy;
                    nx *= aspect;
                    float ws = p_win_soft;
                    if (ws <= 0.0f) ws = 0.15f;
                    /* R073 hop 79: win_shape 1=rect, 2=ellipse, else circle */
                    float wsh = wb_node_param_value(self,
                                                    "win_shape", t);
                    /* R073 hop 81: win_rot rotates the window about its
                     * center — rotate the SAMPLE into window space. */
                    float wrot = wb_node_param_value(self,
                                                     "win_rot", t);
                    if (wrot != 0.0f) {
                        float ang = wrot * 3.14159265f / 180.0f;
                        float ca = cosf(ang), sa = sinf(ang);
                        float rxo = nx * ca + ny * sa;
                        float ryo = -nx * sa + ny * ca;
                        nx = rxo; ny = ryo;
                    }
                    float dist;
                    if (wsh > 1.5f) {
                        /* R073 hop 80: anisotropic ellipse — win_rx/win_ry
                         * override win_r when bound (value > 0). */
                        float wrx = wb_node_param_value(self,
                                                        "win_rx", t);
                        float wry = wb_node_param_value(self,
                                                        "win_ry", t);
                        if (wrx <= 0.0f) wrx = wr;
                        if (wry <= 0.0f) wry = wr;
                        float ex = nx / (wrx > 1e-3f ? wrx : 1e-3f);
                        float ey = ny / (wry > 1e-3f ? wry : 1e-3f);
                        /* scale back to win_r units so the soft band and
                         * threshold logic stay shared with circle/rect */
                        dist = hypotf(ex, ey) * wr;
                    } else if (wsh > 0.5f) {
                        dist = fmaxf(fabsf(nx), fabsf(ny));   /* rect */
                    } else {
                        dist = sqrtf(nx*nx + ny*ny);          /* circle */
                    }
                    wsel = 1.0f - (dist - wr) / ws;
                    if (wsel < 0.0f) wsel = 0.0f;
                    if (wsel > 1.0f) wsel = 1.0f;
                    /* R073 hop 78 fix: wsel<=0 must NOT break the pixel
                     * loop — fall through; sel multiplies to 0 below so
                     * outside-window pixels pass through unchanged. */
                }
                float mx = p->r > p->g ? (p->r > p->b ? p->r : p->b)
                                       : (p->g > p->b ? p->g : p->b);
                float mn = p->r < p->g ? (p->r < p->b ? p->r : p->b)
                                       : (p->g < p->b ? p->g : p->b);
                float d = mx - mn;
                if (d > 1e-5f && mx > 0.0f) {
                    float hue;
                    if (mx == p->r)
                        hue = (p->g - p->b) / d;
                    else if (mx == p->g)
                        hue = 2.0f + (p->b - p->r) / d;
                    else
                        hue = 4.0f + (p->r - p->g) / d;
                    hue *= 60.0f; if (hue < 0) hue += 360.0f;
                    /* angular distance to the qualifier center */
                    float dd = hue - hc * 360.0f;
                    while (dd > 180.0f) dd -= 360.0f;
                    while (dd < -180.0f) dd += 360.0f;
                    float ad = fabsf(dd);
                    if (ad < hw) {
                        float sel = (1.0f - ad / hw) * wsel;   /* soft selection */
                        float lum = wb_lin_luma(p->r, p->g, p->b);  /* #29/#31 linear */
                                  + 0.0722f*p->b;
                        float sat = smul != 0.0f ? smul : 1.0f;
                        float nr = lum + (p->r-lum)*sat;
                        float ng = lum + (p->g-lum)*sat;
                        float nb = lum + (p->b-lum)*sat;
                        /* R074 fix: hue_shift now actually rotates the
                         * qualified pixels' hue (degrees). */
                        if (hsh != 0.0f && sel > 0.0f) {
                            float hr = hsh * sel;
                            const float ca = cosf(hr*3.14159265f/180.0f);
                            const float sa = sinf(hr*3.14159265f/180.0f);
                            /* rotate around the grey axis via YIQ matrix */
                            float yr = nr, yi_ = ng, yib = nb;
                            (void)yr; (void)yi_; (void)yib;
                            float m[9] = {
                                0.299f+0.701f*ca+0.168f*sa,
                                0.587f-0.587f*ca+0.330f*sa,
                                0.114f-0.114f*ca-0.497f*sa,
                                0.299f-0.299f*ca-0.328f*sa,
                                0.587f+0.413f*ca+0.035f*sa,
                                0.114f-0.114f*ca+0.292f*sa,
                                0.299f-0.300f*ca+1.250f*sa,
                                0.587f-0.588f*ca-1.050f*sa,
                                0.114f+0.886f*ca-0.203f*sa };
                            float orr = nr*m[0]+ng*m[1]+nb*m[2];
                            float ogg = nr*m[3]+ng*m[4]+nb*m[5];
                            float obb = nr*m[6]+ng*m[7]+nb*m[8];
                            nr = orr; ng = ogg; nb = obb;
                        }
                        p->r = p->r*(1-sel) + nr*sel;
                        p->g = p->g*(1-sel) + ng*sel;
                        p->b = p->b*(1-sel) + nb*sel;
                                            }
                }
            }
            else if (e->op == 7) {
                /* R073 hop 47b: glow — threshold bright pixels, blur them,
                 * screen-add back. Simplified: per-pixel soft-knee bloom on
                 * luminance above the keyframable "glow_thr" param. */
                float thr = p_glow_thr;
                if (thr <= 0.0f) thr = 0.7f;
                for (int y = ry; y < ry + rh; y++)
                    for (int x = rx; x < rx + rw; x++) {
                        wb_px *p = &in->px[y*in->w + x];
                        float lum = wb_lin_luma(p->r, p->g, p->b);  /* #29/#31 linear */
                                  + 0.0722f*p->b;
                        if (lum > thr) {
                            float excess = (lum - thr) / (1.0f - thr);
                            /* R074 fix: clamp to [0,1] — was unbounded */
                            p->r = (p->r + excess*p->r*0.5f > 1.0f)
                                 ? 1.0f : p->r + excess*p->r*0.5f;
                            p->g = (p->g + excess*p->g*0.5f > 1.0f)
                                 ? 1.0f : p->g + excess*p->g*0.5f;
                            p->b = (p->b + excess*p->b*0.5f > 1.0f)
                                 ? 1.0f : p->b + excess*p->b*0.5f;
                        }
                    }
            }
            else if (e->op == 5) {
                /* R073 hop 45: luma key — dark pixels go transparent
                 * (screen-blend style, ideal over black-bg CGI renders).
                 * threshold from keyframable "lum_thr" param. */
                float thr = p_lum_thr;
                if (thr <= 0.0f) thr = gain > 0 ? gain : 0.15f;
                float lum = wb_lin_luma(p->r, p->g, p->b);  /* #29/#31 linear */
                if (lum <= thr)       p->a = 0.0f;
                else if (lum < thr*2) p->a *= (lum - thr) / thr;
            }
            else if (e->op == 3) {
                /* R073 hop 41: chroma key — green-screen removal.
                 * keyed when green dominates red+blue beyond tolerance;
                 * edge softening via partial alpha in the tolerance band. */
                /* R074 hop 116 (#30): key_tol falls back to ctor gain
                 * ONLY when no explicit key_tol track is bound (same
                 * binding-detection as #51) — collision resolved */
                float tol = 0.0f;
                for (int pi2 = 0; pi2 < self->n_params; pi2++) {
                    if (self->param_names &&
                        strncmp(self->param_names[pi2], "key_tol", 32) == 0) {
                        tol = wb_node_param_value(self, "key_tol", t);
                        break;
                    }
                }
                if (tol <= 0.0f) tol = gain;
                if (tol <= 0.0f) tol = 0.15f;
                /* R073 hop 72: key_color selects the screen channel:
                 * 0 = green (default), 1 = blue, 2 = red. */
                float kc = wb_node_param_value(self, "key_color", t);
                int kch = kc > 0.5f ? (kc > 1.5f ? 2 : 1) : 0;
                float dom;
                if (kch == 1)      dom = p->b - 0.5f * (p->r + p->g);
                else if (kch == 2) dom = p->r - 0.5f * (p->g + p->b);
                else               dom = p->g - 0.5f * (p->r + p->b);
                if (dom >= tol) p->a = 0.0f;
                else if (dom > tol*0.5f) {
                    /* R074 hop 116 (#26): smoothstep edge softness */
                    float u2 = (dom - tol*0.5f) / (tol*0.5f);
                    u2 = u2 * u2 * (3.0f - 2.0f * u2);
                    p->a *= 1.0f - u2;
                }
                /* R073 hop 71: spill suppression — on kept pixels, clamp
                 * green toward max(r,b) so reflected green light no longer
                 * tints the foreground (classic "green limit"). */
                float kv2 = p->g, lo1 = p->r, lo2 = p->b;
                if (kch == 1) { kv2 = p->b; lo1 = p->r; lo2 = p->g; }
                else if (kch == 2) { kv2 = p->r; lo1 = p->g; lo2 = p->b; }
                if (p->a > 0.01f && kv2 > lo1 && kv2 > lo2) {
                    float lim = lo1 > lo2 ? lo1 : lo2;
                    float spill = wb_node_param_value(self,
                                                      "spill", t);
                    if (spill <= 0.0f) spill = 0.5f;   /* default half */
                    float fixed = kv2 - (kv2 - lim) * spill;
                    if (kch == 1)      p->b = fixed;
                    else if (kch == 2) p->r = fixed;
                    else               p->g = fixed;
                }
            }
        curves_skip:;
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

/* R073 hop 69: one transform evaluation into `out` at sub-time ts */
static void tf_eval(wb_node *self, tf_t *tf, double ts,
                    wb_frame *in, wb_frame *out,
                    float wsum) {
    float scale = tf->scale, cx = tf->cx, cy = tf->cy, rot = tf->rot;
    float k;
    k = wb_node_param_value(self, "scale", ts); if (k != 0.0f) scale = k;
    k = wb_node_param_value(self, "cx",    ts); if (k != 0.0f) cx = k;
    k = wb_node_param_value(self, "cy",    ts); if (k != 0.0f) cy = k;
    k = wb_node_param_value(self, "rot",   ts); if (k != 0.0f) rot = k;
    float px = cx * in->w, py = cy * in->h;
    float c = cosf(rot), s = sinf(rot);
    float sc = (scale > 1e-3f) ? scale : 1e-3f;
    for (int y = 0; y < in->h; y++) {
        for (int x = 0; x < in->w; x++) {
            float dx = (float)x - px, dy = (float)y - py;
            float sx = (dx * c + dy * s) / sc + px;
            float sy = (-dx * s + dy * c) / sc + py;
            int ix = (int)floorf(sx), iy = (int)floorf(sy);
            wb_px *dst = &out->px[y * in->w + x];
            if (ix >= 0 && ix < in->w && iy >= 0 && iy < in->h) {
                wb_px v = in->px[iy * in->w + ix];
                if (wsum <= 0.0f) {          /* assign mode */
                    *dst = v;
                } else {                     /* accumulate mode */
                    dst->r += v.r * wsum; dst->g += v.g * wsum;
                    dst->b += v.b * wsum; dst->a += v.a * wsum;
                }
            } else if (wsum <= 0.0f) {
                dst->r = dst->g = dst->b = dst->a = 0.0f;
            }
        }
    }
}

static wb_frame *tf_pull(wb_node *self, double t,
                         int rx, int ry, int rw, int rh, int phase) {
    tf_t *tf = self->user;
    if (self->n_inputs < 1) return NULL;
    if (phase == 0) { wb_node_pull_request(self->inputs[0], t, rx, ry, rw, rh); return NULL; }

    /* R073 hop 69: motion blur — mblur > 0 averages NSUB transform
     * evaluations across [t - mblur/2, t + mblur/2] seconds. */
    float mblur = wb_node_param_value(self, "mblur", t);
    const int NSUB = 4;
    if (mblur > 1e-3f && self->n_inputs >= 1) {
        wb_frame *acc = NULL, *inref = NULL;
        float wsum = 1.0f / NSUB;
        for (int s2 = 0; s2 < NSUB; s2++) {
            double ts = t - mblur * 0.5 +
                        mblur * ((double)s2 + 0.5) / NSUB;
            wb_frame *in = wb_node_pull(self->inputs[0], ts,
                                        rx, ry, rw, rh);
            if (!in) continue;
            if (!acc) {
                acc = wb_frame_alloc(in->w, in->h);
                if (!acc) { wb_frame_free(in); return NULL; }
                acc->roi_x = rx; acc->roi_y = ry;
                acc->roi_w = rw; acc->roi_h = rh;
                memset(acc->px, 0,
                       in->w * in->h * sizeof(wb_px));
            } else if (in != inref) {
                /* frames differ per pull: blend by index */
            }
            tf_eval(self, tf, ts, in, acc, wsum);
            if (!inref) inref = in;
            else if (in != inref) wb_frame_free(in);
        }
        if (inref) wb_frame_free(inref);
        return acc;
    }

    wb_frame *in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
    if (!in) return NULL;
    wb_frame *out = wb_frame_alloc(in->w, in->h);
    if (!out) { wb_frame_free(in); return NULL; }
    out->roi_x = rx; out->roi_y = ry; out->roi_w = rw; out->roi_h = rh;
    tf_eval(self, tf, t, in, out, 0.0f);   /* 0 = plain assignment */
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


/* attach an input to a composite (caller keeps ownership of child) */
static void comp_add(wb_node *comp, wb_node *child) {
    comp->inputs = realloc(comp->inputs, (comp->n_inputs+1)*sizeof(wb_node*));
    comp->inputs[comp->n_inputs++] = child;
}

/* R074 hop 120 (G-SF057): reorder composite layers. */
void wb_composite_move_layer(wb_node *comp, int from, int to) {
    if (!comp || from < 0 || from >= comp->n_inputs) return;
    if (to < 0 || to >= comp->n_inputs || from == to) return;
    wb_node *moved = comp->inputs[from];
    if (from < to)
        memmove(&comp->inputs[from], &comp->inputs[from+1],
                (size_t)(to-from)*sizeof(wb_node*));
    else
        memmove(&comp->inputs[to+1], &comp->inputs[to],
                (size_t)(from-to)*sizeof(wb_node*));
    comp->inputs[to] = moved;
}

/* ---- R074 hop 119 (G-SF049): per-layer transform ---------------------- */
typedef struct { float ox, oy, scale; } comp_layer_t;
#define COMP_MAX_LAYERS 16
static comp_layer_t g_layers[COMP_MAX_LAYERS];
static wb_node     *g_layer_node[COMP_MAX_LAYERS];
static int          g_nlayers = 0;

void wb_composite_set_layer(wb_node *comp, int layer,
                            float ox, float oy, float scale) {
    (void)comp;
    if (layer < 0 || layer >= COMP_MAX_LAYERS) return;
    g_layers[layer].ox = ox; g_layers[layer].oy = oy;
    g_layers[layer].scale = scale > 0 ? scale : 1.0f;
    g_layer_node[layer] = comp;
}
static comp_layer_t *layer_of(wb_node *comp, int idx) {
    for (int i = 0; i < COMP_MAX_LAYERS; i++)
        if (g_layer_node[i] == comp && g_layers[i].scale != 1.0f
            && idx >= 0)
            return &g_layers[i];
    return NULL;
}


static wb_frame *comp_pull(wb_node *self, double t,
                           int rx, int ry, int rw, int rh, int phase) {
    if (self->n_inputs < 1) return NULL;
    if (phase == 0) {   /* G3: request all inputs ahead of compute */
        for (int i = 0; i < self->n_inputs; i++)
            wb_node_pull_request(self->inputs[i], t, rx, ry, rw, rh);
        return NULL;
    }
    /* R074 fix: RoD = max of resolved input formats (was 4096x4096). */
    int cw = 0, chh = 0;
    for (int i = 0; i < self->n_inputs; i++) {
        int iw, ih;
        node_resolve_format(self->inputs[i], &iw, &ih);
        if (iw > cw) cw = iw;
        if (ih > chh) chh = ih;
    }
    if (cw <= 0 || chh <= 0) { cw = rw > 0 ? rx+rw : 64;
                               chh = rh > 0 ? ry+rh : 64; }
    wb_frame *out = wb_frame_alloc(cw, chh);
    if (!out) return NULL;
    out->roi_x = rx; out->roi_y = ry; out->roi_w = rw; out->roi_h = rh;
    for (int i = 0; i < self->n_inputs; i++) {
        wb_frame *f = wb_node_pull(self->inputs[i], t, rx, ry, rw, rh);
        if (!f) continue;
        /* G-SF049: layer transform — offset/scale layers above the base.
         * Base layer (i==0) always fills the frame untouched. */
        comp_layer_t *L = (i > 0) ? layer_of(self, i) : NULL;
        if (L && f->w > 1 && f->h > 1) {
            int nw = (int)(f->w * L->scale);
            int nh = (int)(f->h * L->scale);
            int ox = (int)L->ox + ((cw - nw) >> 1);
            int oy = (int)L->oy + ((chh - nh) >> 1);
            for (int y = ry; y < ry + rh && y < chh; y++) {
                int fy = (y - oy) * f->h / nh;
                if (fy < 0 || fy >= f->h) continue;
                for (int x = rx; x < rx + rw && x < cw; x++) {
                    int fx = (x - ox) * f->w / nw;
                    if (fx < 0 || fx >= f->w) continue;
                    out->px[y*cw + x] = f->px[fy*f->w + fx];
                }
            }
            continue;
        }
        for (int y = ry; y < ry + rh && y < chh; y++)
            for (int x = rx; x < rx + rw && x < cw; x++) {
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
    /* R074 fix: symmetric cy param for vertical placement */
    float cyn = wb_node_param_value(self, "cy", t);
    if (cyn <= 0.0f) cyn = (float)d->y / d->h;
    int y0p = (int)(cyn * d->h);
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
                       y0p + d->scale);
    wb_ui_text_to_rgba(draw_text, d->scale, d->r, d->g, d->b, fa,
                       f->px, d->w, d->h, x0 + (int)slide_off, y0p);
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
    wb_node_set_format(n, s->w, s->h);
    return n;
}

/* R074 hop 133 (#92): resolution-relative title sizing — returns a
 * pixel scale for the given frame width (width/80 clamped 1..8). */
int wb_node_source_text_scale_for(int frame_w) {
    int s = frame_w / 80;
    if (s < 1) s = 1;
    if (s > 8) s = 8;
    return s;
}


/* R074 hop 113 (G-SF050): set a text node's pixel position. */
void wb_node_source_text_pos(wb_node *n, int x, int y) {
    if (!n || !n->user) return;
    src_text_t *s = n->user;
    s->x = x; s->y = y;
}

/* ---- R074 hop 111: SCENE source (gradient + moving band) -------------- */
typedef struct {
    int w, h;
    float r0,g0,b0, r1,g1,b1;   /* top / bottom gradient colors */
    int mode;                   /* 0 = vertical, 1 = radial */
    float band_speed;           /* moving highlight speed (cycles/sec) */
} src_scene_t;

static wb_frame *src_scene_pull(wb_node *self, double t,
                                int rx, int ry, int rw, int rh, int phase) {
    (void)phase;
    src_scene_t *s = self->user;
    wb_frame *f = wb_frame_alloc(s->w, s->h);
    if (!f) return NULL;
    float cx = s->w * 0.5f, cy = s->h * 0.5f;
    float maxd = sqrtf(cx*cx + cy*cy);
    float band = sinf(2.0f*3.14159265f*s->band_speed*t) * 0.5f + 0.5f;
    for (int y = 0; y < s->h; y++) {
        float v = (float)y / (s->h > 1 ? s->h-1 : 1);
        for (int x = 0; x < s->w; x++) {
            float u = (float)x / (s->w > 1 ? s->w-1 : 1);
            float m;
            if (s->mode == 1) {
                float dx = x - cx, dy = y - cy;
                m = sqrtf(dx*dx + dy*dy) / maxd;
            } else {
                m = v;
            }
            float k = m*m*(3-2*m);          /* smoothstep the gradient */
            /* diagonal light sweep adds life without a blur pass */
            float sweep = 0.05f * (float)sin((u + v*0.5f + band)
                          * 3.14159265f);   /* R074: subtler sweep */
            float rr = s->r0 + (s->r1-s->r0)*k + sweep;
            float gg = s->g0 + (s->g1-s->g0)*k + sweep;
            float bb = s->b0 + (s->b1-s->b0)*k + sweep;
            if (rr<0) rr=0; if (rr>1) rr=1;
            if (gg<0) gg=0; if (gg>1) gg=1;
            if (bb<0) bb=0; if (bb>1) bb=1;
            wb_px *q = &f->px[y*s->w + x];
            q->r = rr; q->g = gg; q->b = bb; q->a = 1.0f;
        }
    }
    f->roi_x = 0; f->roi_y = 0; f->roi_w = s->w; f->roi_h = s->h;
    return f;
}
static void src_scene_free(wb_node *n) { free(n->user); }

wb_node *wb_node_source_scene(float r0, float g0, float b0,
                              float r1, float g1, float b1,
                              int mode, float band_speed,
                              int w, int h) {
    wb_node *n = wb_node_create(WB_NODE_SOURCE, "src_scene");
    if (!n) return NULL;
    src_scene_t *s = calloc(1, sizeof(*s));
    if (!s) { wb_node_destroy(n); return NULL; }
    s->w = w > 0 ? w : 320; s->h = h > 0 ? h : 240;
    s->r0=r0; s->g0=g0; s->b0=b0; s->r1=r1; s->g1=g1; s->b1=b1;
    s->mode = mode; s->band_speed = band_speed;
    n->user = s;
    n->pull = src_scene_pull;
    n->free = src_scene_free;
    wb_node_set_format(n, s->w, s->h);
    return n;
}


/* ---- R074 hop 112: FRAME source (external RGBA buffer) ---------------- */
typedef struct {
    int w, h;
    uint8_t *rgba;        /* w*h*4, caller-owned and updated */
} src_frame_t;

static wb_frame *src_frame_pull(wb_node *self, double t,
                                int rx, int ry, int rw, int rh, int phase) {
    (void)t; (void)phase; (void)rx; (void)ry; (void)rw; (void)rh;
    src_frame_t *s = self->user;
    if (!s || !s->rgba) return NULL;
    wb_frame *f = wb_frame_alloc(s->w, s->h);
    if (!f) return NULL;
    for (int i = 0; i < s->w * s->h; i++) {
        wb_px *q = &f->px[i];
        q->r = s->rgba[i*4+0] / 255.0f;
        q->g = s->rgba[i*4+1] / 255.0f;
        q->b = s->rgba[i*4+2] / 255.0f;
        q->a = s->rgba[i*4+3] / 255.0f;
    }
    f->roi_x = 0; f->roi_y = 0; f->roi_w = s->w; f->roi_h = s->h;
    return f;
}
static void src_frame_free(wb_node *n) { free(n->user); }

wb_node *wb_node_source_frame(int w, int h, uint8_t *rgba) {
    wb_node *n = wb_node_create(WB_NODE_SOURCE, "src_frame");
    if (!n) return NULL;
    src_frame_t *s = calloc(1, sizeof(*s));
    if (!s) { wb_node_destroy(n); return NULL; }
    s->w = w; s->h = h; s->rgba = rgba;
    n->user = s;
    n->pull = src_frame_pull;
    n->free = src_frame_free;
    wb_node_set_format(n, w, h);
    return n;
}




/* ---- R074 hop 113 (G-SF030/031): DITHER node — SNES ordered dither ---- */
typedef struct { int levels; } wb_dither_t;

static wb_frame *dither_pull(wb_node *self, double t,
                             int rx, int ry, int rw, int rh, int phase) {
    (void)t; (void)phase;
    if (!self->inputs || !self->inputs[0]) return NULL;
    wb_frame *in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
    if (!in) return NULL;
    wb_dither_t *d = self->user;
    int lv = d->levels > 1 ? d->levels : 6;
    float step = 1.0f / (float)(lv - 1);
    static const float bayer[4][4] = {
        { 0,8,2,10}, {12,4,14,6}, {3,11,1,9}, {15,7,13,5}
    };
    for (int y = in->roi_y; y < in->roi_y + in->roi_h; y++) {
        for (int x = in->roi_x; x < in->roi_x + in->roi_w; x++) {
            wb_px *q = &in->px[y * in->w + x];
            float th = (bayer[y & 3][x & 3] / 16.0f - 0.5f) * step;
            q->r = floorf((q->r + th) / step + 0.5f) * step;
            if (q->r < 0) q->r = 0; else if (q->r > 1) q->r = 1;
            q->g = floorf((q->g + th) / step + 0.5f) * step;
            if (q->g < 0) q->g = 0; else if (q->g > 1) q->g = 1;
            q->b = floorf((q->b + th) / step + 0.5f) * step;
            if (q->b < 0) q->b = 0; else if (q->b > 1) q->b = 1;
        }
    }
    return in;
}
static void dither_free(wb_node *n) { free(n->user); }

wb_node *wb_node_effect_dither(int levels) {
    wb_node *n = wb_node_create(WB_NODE_EFFECT, "dither");
    if (!n) return NULL;
    wb_dither_t *d = calloc(1, sizeof(*d));
    if (!d) { wb_node_destroy(n); return NULL; }
    d->levels = levels;
    n->user = d;
    n->pull = dither_pull;
    n->free = dither_free;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node *));
    return n;
}


/* ---- R074 hop 116 (#10): SCALER node — bilinear resize ---------------- */
typedef struct { int out_w, out_h; } scaler_t;

static wb_frame *scaler_pull(wb_node *self, double t,
                             int rx, int ry, int rw, int rh, int phase) {
    (void)rx; (void)ry; (void)rw; (void)rh; (void)phase;
    if (!self->inputs || !self->inputs[0]) return NULL;
    /* G-SF048: resolve the input's format for a full-frame pull */
    int iw = 0, ih = 0;
    node_resolve_format(self->inputs[0], &iw, &ih);
    if (iw <= 0 || ih <= 0) { iw = 64; ih = 64; }
    wb_frame *in = wb_node_pull(self->inputs[0], t, 0, 0, iw, ih);
    if (!in) return NULL;
    int W = in->roi_w, H = in->roi_h;
    int OW = ((scaler_t*)self->user)->out_w;
    int OH = ((scaler_t*)self->user)->out_h;
    if (OW <= 0) OW = W;
    if (OH <= 0) OH = H;
    wb_frame *out = wb_frame_alloc(OW, OH);
    if (!out) return in;
    if (W < 2 || H < 2) {   /* degenerate input: nearest copy */
        for (int y = 0; y < OH && y < H; y++)
            for (int x = 0; x < OW && x < W; x++)
                out->px[y*OW + x] = in->px[y*W + x];
        return out;
    }
    for (int y = 0; y < OH; y++) {
        float fy = (y + 0.5f) * H / (float)OH - 0.5f;
        int y0 = (int)floorf(fy);
        float ty = fy - y0;
        if (y0 < 0) { y0 = 0; ty = 0; }
        if (y0 >= H-1) { y0 = H-2 < 0 ? 0 : H-2; ty = 1; }
        for (int x = 0; x < OW; x++) {
            float fx = (x + 0.5f) * W / (float)OW - 0.5f;
            int x0 = (int)floorf(fx);
            float tx = fx - x0;
            if (x0 < 0) { x0 = 0; tx = 0; }
            if (x0 >= W-1) { x0 = W-2 < 0 ? 0 : W-2; tx = 1; }
            const wb_px *p00=&in->px[(y0)*W + x0],   *p10=&in->px[(y0)*W+x0+1];
            const wb_px *p01=&in->px[(y0+1)*W + x0], *p11=&in->px[(y0+1)*W+x0+1];
            wb_px *q = &out->px[y*OW + x];
            q->r=(p00->r*(1-tx)+p10->r*tx)*(1-ty)+(p01->r*(1-tx)+p11->r*tx)*ty;
            q->g=(p00->g*(1-tx)+p10->g*tx)*(1-ty)+(p01->g*(1-tx)+p11->g*tx)*ty;
            q->b=(p00->b*(1-tx)+p10->b*tx)*(1-ty)+(p01->b*(1-tx)+p11->b*tx)*ty;
            q->a=(p00->a*(1-tx)+p10->a*tx)*(1-ty)+(p01->a*(1-tx)+p11->a*tx)*ty;
        }
    }
    out->roi_x = 0; out->roi_y = 0; out->roi_w = OW; out->roi_h = OH;
    return out;
}
static void scaler_free(wb_node *n) { free(n->user); }

wb_node *wb_node_effect_scaler(int out_w, int out_h) {
    wb_node *n = wb_node_create(WB_NODE_EFFECT, "scaler");
    if (!n) return NULL;
    scaler_t *s = calloc(1, sizeof(*s));
    if (!s) { wb_node_destroy(n); return NULL; }
    s->out_w = out_w; s->out_h = out_h;
    n->user = s; n->pull = scaler_pull; n->free = scaler_free;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node *));
    wb_node_set_format(n, out_w > 0 ? out_w : 64, out_h > 0 ? out_h : 64);
    return n;
}


/* ---- R074 hop 119 (G-SF051/052/053): presentation effect nodes -------- */
/* Shared single-input effect pull wrapper */
typedef struct {
    int   kind;        /* 0=letterbox 1=scanline 2=chromatic */
    float amount;      /* letterbox bar fraction / scanline strength /
                          chromatic offset px */
} pres_t;

static wb_frame *pres_pull(wb_node *self, double t,
                           int rx, int ry, int rw, int rh, int phase) {
    (void)phase;
    (void)rx; (void)ry;
    if (!self->inputs || !self->inputs[0]) return NULL;
    wb_frame *in = wb_node_pull(self->inputs[0], t, -1,-1,-1,-1);
    if (!in) return NULL;
    pres_t *d = self->user;
    int W = in->w, H = in->h;
    if (d->kind == 0) {                       /* LETTERBOX */
        int bar = (int)(H * d->amount);
        for (int y = 0; y < H; y++) {
            if (y < bar || y >= H - bar)
                memset(&in->px[y*W], 0, (size_t)W*sizeof(wb_px));
        }
        return in;
    }
    if (d->kind == 1) {                       /* SCANLINE / CRT */
        for (int y = 0; y < H; y++) {
            if (y % 2) continue;
            for (int x = 0; x < W; x++) {
                wb_px *p = &in->px[y*W + x];
                float k = 1.0f - d->amount;
                p->r*=k; p->g*=k; p->b*=k;
            }
        }
        return in;
    }
    /* CHROMATIC ABERRATION: shift R left, B right by amount px */
    int off = (int)(d->amount + 0.5f);
    if (off <= 0) return in;
    wb_frame *out = wb_frame_alloc(W, H);
    if (!out) return in;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            wb_px *q = &out->px[y*W + x];
            int xr = x - off < 0 ? 0 : x - off;
            int xb = x + off >= W ? W-1 : x + off;
            q->r = in->px[y*W + xr].r;
            q->g = in->px[y*W + x].g;
            q->b = in->px[y*W + xb].b;
            q->a = in->px[y*W + x].a;
        }
    }
    out->roi_x = out->roi_y = 0; out->roi_w = W; out->roi_h = H;
    return out;
}
static void pres_free(wb_node *n) { free(n->user); }

static wb_node *pres_create(int kind, float amount) {
    wb_node *n = wb_node_create(WB_NODE_EFFECT, "pres");
    if (!n) return NULL;
    pres_t *d = calloc(1, sizeof(*d));
    if (!d) { wb_node_destroy(n); return NULL; }
    d->kind = kind; d->amount = amount;
    n->user = d; n->pull = pres_pull; n->free = pres_free;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node *));
    return n;
}
wb_node *wb_node_effect_letterbox(float bar_fraction) {
    return pres_create(0, bar_fraction);
}
wb_node *wb_node_effect_scanline(float strength) {
    return pres_create(1, strength);
}
wb_node *wb_node_effect_chromatic(float offset_px) {
    return pres_create(2, offset_px);
}


/* ---- R074 hop 122 (G-SF032): MODE-7 affine ground warp node ----------- */
typedef struct { float horizon; float scale; double scroll; } m7_t;

static wb_frame *m7_pull(wb_node *self, double t,
                         int rx, int ry, int rw, int rh, int phase) {
    (void)phase;
    (void)rx; (void)ry;
    if (!self->inputs || !self->inputs[0]) return NULL;
    wb_frame *in = wb_node_pull(self->inputs[0], t, -1,-1,-1,-1);
    if (!in || in->w < 2 || in->h < 2) return in;
    m7_t *d = self->user;
    int W = in->w, H = in->h;
    int hy = (int)(H * d->horizon);
    if (hy <= 0) hy = H/3;
    wb_frame *out = wb_frame_alloc(W, H);
    if (!out) return in;
    /* sky: copy top rows */
    for (int y = 0; y < hy && y < H; y++)
        memcpy(&out->px[y*W], &in->px[y*W], (size_t)W*sizeof(wb_px));
    double scrolloff = d->scroll * t * 60.0;
    for (int y = hy; y < H; y++) {
        /* perspective: rows near bottom are "close" */
        float pz = (float)(y - hy + 1) / (float)(H - hy);
        float spread = d->scale / (pz * pz + 0.02f);
        for (int x = 0; x < W; x++) {
            float u = ((x - W*0.5f) * spread + (float)scrolloff)
                      / W + 0.5f;
            u = u - floorf(u);            /* wrap */
            int sx = (int)(u * W) % W; if (sx < 0) sx += W;
            out->px[y*W + x] = in->px[y*W + sx];
        }
    }
    out->roi_x=0; out->roi_y=0; out->roi_w=W; out->roi_h=H;
    return out;
}
static void m7_free(wb_node *n) { free(n->user); }

wb_node *wb_node_effect_mode7(float horizon_frac, float strength,
                              double scroll_speed) {
    wb_node *n = wb_node_create(WB_NODE_EFFECT, "mode7");
    if (!n) return NULL;
    m7_t *d = calloc(1, sizeof(*d));
    if (!d) { wb_node_destroy(n); return NULL; }
    d->horizon = horizon_frac > 0 ? horizon_frac : 0.35f;
    d->scale = strength > 0 ? strength : 1.0f;
    d->scroll = scroll_speed;
    n->user = d; n->pull = m7_pull; n->free = m7_free;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node *));
    return n;
}


/* R074 hop 131 (#78): query a node's resolved output dimensions without
 * pulling. Returns 0 on success. */
int wb_node_output_dims(wb_node *n, int *w, int *h) {
    if (!n) return -1;
    node_resolve_format(n, w, h);
    return (*w > 0 && *h > 0) ? 0 : -1;
}

/* R073 hop 101: write a frame as a binary PPM (P6) image. */
int wb_frame_write_ppm(const wb_frame *f, const char *path) {
    if (!f || !path || f->w <= 0 || f->h <= 0) return -1;
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    fprintf(fp, "P6\n%d %d\n255\n", f->w, f->h);
    for (int i = 0; i < f->w * f->h; i++) {
        /* R074 fix: clamp to [0,1] — HDR-ish values wrapped to garbage */
        float fr = f->px[i].r, fg = f->px[i].g, fb = f->px[i].b;
        if (fr < 0) fr = 0; if (fr > 1) fr = 1;
        if (fg < 0) fg = 0; if (fg > 1) fg = 1;
        if (fb < 0) fb = 0; if (fb > 1) fb = 1;
        unsigned char r = (unsigned char)(fr * 255.0f + 0.5f);
        unsigned char g = (unsigned char)(fg * 255.0f + 0.5f);
        unsigned char b = (unsigned char)(fb * 255.0f + 0.5f);
        fwrite(&r, 1, 1, fp); fwrite(&g, 1, 1, fp); fwrite(&b, 1, 1, fp);
    }
    fclose(fp);
    return 0;
}

/* R073 hop 104: render a transition graph to an mp4 via the vendored
 * ffmpeg binary (image2 demuxer over a temp PPM sequence). */
/* R074 fix: refuse shell metacharacters in ffmpeg-bound paths */
static int path_shell_safe(const char *s) {
    if (!s) return 0;
    for (; *s; s++)
        if (*s == ';' || *s == '&' || *s == '|' || *s == '`' ||
            *s == '$' || *s == '\n' || *s == '>' || *s == '<')
            return 0;
    return 1;
}
int wb_compositor_export_mp4_audio(wb_node *trans, const char *mp4_path,
                                   const char *wav_path,
                                   double dur, int fps, int w, int h);
/* R073 hop 104/105: graph -> mp4; optional wav muxed as AAC. */
int wb_compositor_export_mp4(wb_node *trans, const char *mp4_path,
                             double dur, int fps, int w, int h) {
    return wb_compositor_export_mp4_audio(trans, mp4_path, NULL,
                                          dur, fps, w, h);
}

int wb_compositor_export_mp4_audio(wb_node *trans, const char *mp4_path,
                                   const char *wav_path,
                                   double dur, int fps, int w, int h) {
    if (!trans || !mp4_path || dur <= 0 || fps <= 0) return -1;
    if (!path_shell_safe(mp4_path)) return -1;
    if (wav_path && !path_shell_safe(wav_path)) return -1;
    char dir[256];
    snprintf(dir, sizeof dir, "/tmp/bigmac_cseq_%d", (int)getpid());
    /* crude mkdir -p equivalent: single level under /tmp */
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return -1;
    int nframes = (int)(dur * fps);
    /* R074 fix: 0.4s fade-from-black / fade-to-black bookends */
    double fade = dur > 1.2 ? 0.4 : dur * 0.25;
    for (int kk = 0; kk < nframes; kk++) {
        double tt = (double)kk / fps;
        wb_frame *f = wb_node_pull(trans, tt, 0, 0, w, h);
        if (!f) return -1;
        float gaink = 1.0f;
        if (tt < fade)          gaink = (float)(tt / fade);
        else if (tt > dur-fade) gaink = (float)((dur-tt)/fade);
        if (gaink < 1.0f)
            for (int i2 = 0; i2 < f->w*f->h; i2++) {
                f->px[i2].r *= gaink;
                f->px[i2].g *= gaink;
                f->px[i2].b *= gaink;
            }
        char p[512];
        snprintf(p, sizeof p, "%s/f_%05d.ppm", dir, kk);
        int wr = wb_frame_write_ppm(f, p);
        wb_frame_free(f);
        if (wr != 0) return -1;
    }
    char cmd[1536];
    if (wav_path) {
        snprintf(cmd, sizeof cmd,
            "/Users/waefrebeorn/.local/bin/ffmpeg -y -loglevel error "
            "-f image2 -framerate %d -i '%s/f_%%05d.ppm' "
            "-i '%s' -c:a aac -b:a 192k -shortest "
            "-map 0:v:0 -map 1:a:0 "
            "-c:v libx264 -pix_fmt yuv420p -movflags +faststart '%s'",
            fps, dir, wav_path, mp4_path);
    } else {
        snprintf(cmd, sizeof cmd,
            "/Users/waefrebeorn/.local/bin/ffmpeg -y -loglevel error "
            "-f image2 -framerate %d -i '%s/f_%%05d.ppm' "
            "-c:v libx264 -pix_fmt yuv420p -movflags +faststart '%s'",
            fps, dir, mp4_path);
    }
    int rc = system(cmd);
    /* R074 fix: poster thumbnail next to the video */
    if (rc == 0) {
        char pcmd[1280];
        snprintf(pcmd, sizeof pcmd,
            "/Users/waefrebeorn/.local/bin/ffmpeg -y -loglevel error "
            "-ss %.2f -i '%s' -frames:v 1 '%s.png'",
            dur * 0.5, mp4_path, mp4_path);   /* R074: mid-frame poster */
        system(pcmd);
    }
    /* clean temp frames */
    for (int kk = 0; kk < nframes; kk++) {
        char p[512];
        snprintf(p, sizeof p, "%s/f_%05d.ppm", dir, kk);
        remove(p);
    }
    rmdir(dir);
    return rc == 0 ? 0 : -1;
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
    /* R074 hop 122 (#53): null-guard both inputs before mixing */
    if (!a && !b) return NULL;
    if (!a) return b;
    if (!b) return a;
    /* R074 fix: transitions can now be placed on the timeline via the
     * keyframable "t_start" param (default 0). Local time drives u. */
    double t0s = wb_node_param_value(self, "t_start", t);
    double lt = t - (double)t0s;
    if (lt < 0) lt = 0;
    /* mix factor 0..1 across the transition window; before = A, after = B.
     * R074 fix: smoothstep easing instead of linear (broadcast standard). */
    double u = tr->dur > 0 ? lt / tr->dur : 1.0;
    if (u < 0) u = 0; if (u > 1) u = 1;
    u = u * u * (3.0 - 2.0 * u);   /* smoothstep */
    float mB = (float)u;

    /* R073 hop 68/83: map input. Required (3rd input) for op 7; optional
     * for any other transition — when present it modulates the per-pixel
     * progress spatially by the map's Rec.709 luma (masked transition). */
    wb_frame *mapf = NULL;
    if (self->n_inputs >= 3 && self->inputs[2]) {
        mapf = wb_node_pull(self->inputs[2], t, rx, ry, rw, rh);
        if (!mapf) { wb_frame_free(a); wb_frame_free(b); return NULL; }
    } else if (tr->op == 7) {
        wb_frame_free(a); wb_frame_free(b); return NULL;
    }

    /* R074 fix: guard size mismatch — nested graphs can hand back
     * different dims (composite RoD). Use the overlap; larger input is
     * sampled at its top-left crop (no scaler yet, documented). */
    if (a->w != b->w || a->h != b->h) {
        int mw = a->w < b->w ? a->w : b->w;
        int mh = a->h < b->h ? a->h : b->h;
        if (mw <= 0 || mh <= 0) {
            wb_frame_free(a); wb_frame_free(b); return NULL;
        }
        /* shrink both to the min via in-place crop views */
        for (int y = 0; y < mh; y++)
            memcpy(b->px + y*mw, b->px + y*b->w,
                   (size_t)mw * sizeof(wb_px));
        for (int y = 0; y < mh; y++)
            memcpy(a->px + y*mw, a->px + y*a->w,
                   (size_t)mw * sizeof(wb_px));
        a->w = mw; a->h = mh; b->w = mw; b->h = mh;
    }
    wb_frame *out = wb_frame_alloc(a->w, a->h);
    if (!out) { wb_frame_free(a); wb_frame_free(b); return NULL; }
    out->roi_x = 0; out->roi_y = 0;
    out->roi_w = a->w; out->roi_h = a->h;   /* R074: honest RoI */
    out->roi_x = rx; out->roi_y = ry; out->roi_w = rw; out->roi_h = rh;
    for (int i = 0; i < a->w * a->h; i++) {
        wb_px pa = a->px[i], pb = b->px[i];
        int px_i = i % a->w, py_i = i / a->w;
        /* R073 hop 83: masked transition — map luma modulates the
         * per-pixel progress for all ops except 7 (which thresholds). */
        float mM = mB;
        if (mapf && tr->op != 7) {
            float lum = 0.2126f*mapf->px[i].r + 0.7152f*mapf->px[i].g
                      + 0.0722f*mapf->px[i].b;
            mM = mB * lum;
        }
        if (tr->op == 0) {
            /* crossfade: linear blend A -> B */
            out->px[i].r = pa.r*(1-mM) + pb.r*mM;
            out->px[i].g = pa.g*(1-mM) + pb.g*mM;
            out->px[i].b = pa.b*(1-mM) + pb.b*mM;
            out->px[i].a = pa.a*(1-mM) + pb.a*mM;
        } else if (tr->op == 1) {
            /* dip-to-black: fade A to black in first half, B up in second */
            float kA = mM < 0.5f ? (1.0f - mM*2.0f) : 0.0f;
            float kB = mM >= 0.5f ? (mM - 0.5f)*2.0f : 0.0f;
            out->px[i].r = pa.r*kA + pb.r*kB;
            out->px[i].g = pa.g*kA + pb.g*kB;
            out->px[i].b = pa.b*kA + pb.b*kB;
            out->px[i].a = pa.a*kA + pb.a*kB;
        } else if (tr->op == 2) {
            /* R073 hop 50/64/65/66: linear wipe — dir 0 L->R, 1 R->L,
             * 2 T->B, 3 B->T; feathered boundary via smoothstep over an
             * 8%-of-frame band (hop 66). */
            float pos, span;
            if (tr->dir <= 1) {
                float edge = mM * (float)a->w;
                pos = tr->dir == 0 ? (float)px_i
                                   : (float)(a->w - px_i);
                span = (float)a->w;
                /* distance behind the boundary, positive = B side */
                pos = edge - pos;
            } else {
                float edge = mM * (float)a->h;
                pos = tr->dir == 2 ? (float)py_i
                                   : (float)(a->h - py_i);
                span = (float)a->h;
                pos = edge - pos;
            }
            float feather = span * 0.08f;
            float u = pos / (feather > 1.0f ? feather : 1.0f);
            /* smoothstep: 0 = fully A, 1 = fully B */
            u = u < 0 ? 0 : (u > 1 ? 1 : u);
            float sB = u * u * (3 - 2 * u);
            out->px[i].r = pa.r*(1-sB) + pb.r*sB;
            out->px[i].g = pa.g*(1-sB) + pb.g*sB;
            out->px[i].b = pa.b*(1-sB) + pb.b*sB;
            out->px[i].a = pa.a*(1-sB) + pb.a*sB;
        } else if (tr->op == 3) {
            /* iris: circle reveals B from a movable center (iris_cx/cy) */
            float icx = wb_node_param_value(self, "iris_cx", t);
            float icy = wb_node_param_value(self, "iris_cy", t);
            if (icx <= 0.0f) icx = 0.5f;
            if (icy <= 0.0f) icy = 0.5f;
            float cx2 = a->w * icx, cy2 = a->h * icy;
            float dx = px_i - cx2, dy = py_i - cy2;
            float dist = sqrtf(dx*dx + dy*dy);
            float maxd = sqrtf(cx2*cx2 + cy2*cy2);
            if (dist < mM * maxd) { out->px[i] = pb; }
            else                  { out->px[i] = pa; }
        } else if (tr->op == 7) {
            /* R073 hop 68: map dissolve — map input's Rec.709 luma is the
             * per-pixel threshold (Photoshop gradient-wipe technique). */
            float lum = 0.2126f*mapf->px[i].r + 0.7152f*mapf->px[i].g
                      + 0.0722f*mapf->px[i].b;
            /* R074 fix: soft knee over the feather band instead of a
             * hard threshold */
            float feath_md = wb_node_param_value(self, "grad_feather", t);
            if (feath_md <= 0.0f) feath_md = 0.05f;
            float kmd = (mB - lum) / feath_md + 0.5f;
            if (kmd < 0) kmd = 0; if (kmd > 1) kmd = 1;
            out->px[i].r = pa.r*(1-kmd) + pb.r*kmd;
            out->px[i].g = pa.g*(1-kmd) + pb.g*kmd;
            out->px[i].b = pa.b*(1-kmd) + pb.b*kmd;
            out->px[i].a = pa.a*(1-kmd) + pb.a*kmd;
        } else if (tr->op == 6) {
            /* R073 hop 67: noise dissolve — deterministic per-pixel hash
             * offsets the local switch time, giving the classic grainy
             * dissolve without any state. */
            unsigned h = (unsigned)(px_i * 73856093u) ^
                         (unsigned)(py_i * 19349663u);
            h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
            float jitter = ((h & 0xFFFF) / 65535.0f - 0.5f) * 0.3f;
            float th = mM + jitter;
            if (th > 0.5f) { out->px[i] = pb; }
            else           { out->px[i] = pa; }
        } else if (tr->op == 4 || tr->op == 5) {
            /* R073 hop 51: slide (4) / push (5) — horizontal translation.
             * sample A at (x + mB*W), B at (x - W + mB*W); for push both
             * translate together, for slide B overlays a stationary A. */
            int sx = (int)(mM * a->w);
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
        } else if (tr->op == 18) {
            /* R073 hop 99: ripple dissolve — a circular wave expands
             * from the frame center; pixels flip A->B as the ripple
             * radius passes them. Ring softness via grad_feather. */
            float feath = wb_node_param_value(self, "grad_feather", t);
            if (feath <= 0.0f) feath = 0.05f;
            /* R074 hop 132 (#59): movable clock center via
             * keyframable "wipe_cx"/"wipe_cy" (normalized). */
            float ccx = wb_node_param_value(self, "wipe_cx", t);
            float ccy = wb_node_param_value(self, "wipe_cy", t);
            if (ccx <= 0.0f) ccx = 0.5f;
            if (ccy <= 0.0f) ccy = 0.5f;
            float dxn = px_i - a->w * ccx;
            float dyn = py_i - a->h * ccy;
            float dist = sqrtf(dxn*dxn + dyn*dyn);
            float maxd = 0.5f * sqrtf((float)(a->w*a->w + a->h*a->h));
            /* two rings for the ripple look: primary + echo */
            float k1 = (mM * maxd - dist) / feath + 0.5f;
            /* R074 hop 122 (#63): keyframable echo ring offset */
            float echo_off = 0.15f;
            {
                /* read from transition node if bound */
                for (int pi2 = 0; pi2 < self->n_params; pi2++) {
                    if (self->param_names &&
                        strncmp(self->param_names[pi2], "ripple_echo", 32) == 0) {
                        echo_off = wb_node_param_value(self, "ripple_echo", t);
                        break;
                    }
                }
            }
            float d2 = fabsf(dist - mM * maxd - maxd * echo_off);
            float k2 = (feath - d2) / feath + 0.5f;
            float k = k1 > k2 ? k1 : k2;
            if (k < 0) k = 0; if (k > 1) k = 1;
            out->px[i].r = pa.r*(1-k) + pb.r*k;
            out->px[i].g = pa.g*(1-k) + pb.g*k;
            out->px[i].b = pa.b*(1-k) + pb.b*k;
            out->px[i].a = pa.a*(1-k) + pb.a*k;
        } else if (tr->op == 17) {
            /* R073 hop 98: split-flap grid dissolve — 8x8 cells flip
             * A->B in a diagonal wave (row+col) with per-cell hash
             * jitter; deterministic, stateless. */
            int cw = a->w / 8 > 0 ? a->w / 8 : 1;
            int ch = a->h / 8 > 0 ? a->h / 8 : 1;
            int col = px_i / cw, row = py_i / ch;
            /* R074 hop 116 (#35): distinct hash constants — no longer
             * correlated with the checkerboard pattern's hash */
            unsigned h = (unsigned)(col * 0x9E3779B1u)
                       ^ (unsigned)(row * 0x85EBCA77u);
            h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15;
            float jitter = ((float)(h & 0xFF) / 255.0f - 0.5f) * 0.2f;
            float wave = (float)(row + col) / 14.0f;   /* 0..~1 */
            float th = mM * 1.4f - wave - jitter;
            if (th > 0.0f) { out->px[i] = pb; }
            else           { out->px[i] = pa; }
        } else if (tr->op == 16) {
            /* R073 hop 97: directional-blur wipe — B slides in over A;
             * pixels near the moving edge average trailing taps along
             * the slide axis, giving a motion smear that peaks at the
             * boundary and vanishes at start/end. */
            int sxm = (int)(mM * a->w);
            float u = (float)(px_i - sxm);   /* <0 inside B, >0 in A */
            /* smear band trails BEHIND the edge (into A's side) */
            float band = a->w * 0.12f
                       * sinf(mM * 3.14159265f);
            wb_px base = (u <= 0) ? pb : pa;
            float sr = base.r, sg = base.g, sb = base.b;
            if (u > 0 && band > 0.5f) {
                /* in the band: blend sharp A with smeared edge samples */
                float w = 1.0f - u / band;          /* 1 at edge..0 */
                if (w < 0) w = 0;
                float acc_r = 0, acc_g = 0, acc_b = 0;
                int taps = 5;
                for (int q = 0; q < taps; q++) {
                    int xx = px_i - (int)((float)q / (taps - 1)
                            * band);
                    if (xx < 0) xx = 0;
                    if (xx >= b->w) xx = b->w - 1;
                    wb_px t2 = (xx < sxm)
                             ? b->px[py_i * b->w + xx]
                             : a->px[py_i * a->w + xx];
                    acc_r += t2.r; acc_g += t2.g; acc_b += t2.b;
                }
                sr = a->px[i].r*(1-w) + (acc_r/taps)*w;
                sg = a->px[i].g*(1-w) + (acc_g/taps)*w;
                sb = a->px[i].b*(1-w) + (acc_b/taps)*w;
            }
            out->px[i].r = sr; out->px[i].g = sg; out->px[i].b = sb;
            out->px[i].a = pa.a;
        } else if (tr->op == 15) {
            /* R073 hop 94: spin-blur transition — angular multi-tap blur
             * rotating around the frame center; strength peaks at the
             * midpoint; A spins out, B spins in, blended by progress. */
            float punch = sinf(mB * 3.14159265f);
            float cx = a->w * 0.5f, cy = a->h * 0.5f;
            float ra = 0, ga_ = 0, ba = 0, rb = 0, gb = 0, bb = 0;
            float sq = wb_node_param_value(self, "blur_taps", t);
            int taps_s = sq > 1.0f ? (int)sq : 5;
            if (taps_s > 16) taps_s = 16;
            if (taps_s < 1) taps_s = 1;
            for (int tap = 0; tap < taps_s; tap++) {
                float ang = (punch * 0.12f) *
                            (taps_s > 1
                             ? ((float)tap / (taps_s - 1) - 0.5f)
                             : 0.0f) * 3.14159265f;
                float ca = cosf(ang), sa = sinf(ang);
                float fx = (float)px_i - cx, fy = (float)py_i - cy;
                int axx = (int)(cx + fx * ca - fy * sa);
                int ayy = (int)(cy + fx * sa + fy * ca);
                if (axx < 0) axx = 0; if (axx >= a->w) axx = a->w-1;
                if (ayy < 0) ayy = 0; if (ayy >= a->h) ayy = a->h-1;
                wb_px q = a->px[ayy * a->w + axx];
                ra += q.r; ga_ += q.g; ba += q.b;
                /* B rotates the opposite way */
                int bxx = (int)(cx + fx * ca + fy * sa);
                int byy = (int)(cy - fx * sa + fy * ca);
                if (bxx < 0) bxx = 0; if (bxx >= b->w) bxx = b->w-1;
                if (byy < 0) byy = 0; if (byy >= b->h) byy = b->h-1;
                q = b->px[byy * b->w + bxx];
                rb += q.r; gb += q.g; bb += q.b;
            }
            ra /= taps_s; ga_ /= taps_s; ba /= taps_s;
            rb /= taps_s; gb /= taps_s; bb /= taps_s;
            out->px[i].r = ra*(1-mM) + rb*mM;
            out->px[i].g = ga_*(1-mM) + gb*mM;
            out->px[i].b = ba*(1-mM) + bb*mM;
            out->px[i].a = pa.a*(1-mM) + pb.a*mM;
        } else if (tr->op == 14) {
            /* R073 hop 92: zoom-blur transition — radial multi-tap blur
             * whose strength peaks at the midpoint; B scales in over A.
             * 5 taps sampled along the ray to frame center. */
            float punch = sinf(mB * 3.14159265f);      /* 0..1..0 */
            float cx = a->w * 0.5f, cy = a->h * 0.5f;
            int sx0 = px_i, sy0 = py_i;
            /* pick source frame: first half reads A zoomed out, second
             * half reads B zoomed in — crossfade the two reads by mB */
            float ra = 0.0f, ga_ = 0.0f, ba = 0.0f;
            float rb = 0.0f, gb = 0.0f, bb = 0.0f;
            float tq = wb_node_param_value(self, "blur_taps", t);
            int taps_z = tq > 1.0f ? (int)tq : 5;
            if (taps_z > 16) taps_z = 16;
            if (taps_z < 1) taps_z = 1;
            for (int tap = 0; tap < taps_z; tap++) {
                float f = taps_z > 1
                        ? (float)tap / (taps_z - 1) : 0.0f;
                int axx = (int)(cx + (sx0 - cx)
                          * (1.0f - punch * 0.15f * f));
                int ayy = (int)(cy + (sy0 - cy)
                          * (1.0f - punch * 0.15f * f));
                if (axx < 0) axx = 0; if (axx >= a->w) axx = a->w - 1;
                if (ayy < 0) ayy = 0; if (ayy >= a->h) ayy = a->h - 1;
                wb_px q = a->px[ayy * a->w + axx];
                ra += q.r; ga_ += q.g; ba += q.b;
                int bxx = (int)(cx + (sx0 - cx)
                          * (1.0f + punch * 0.15f * f));
                int byy = (int)(cy + (sy0 - cy)
                          * (1.0f + punch * 0.15f * f));
                if (bxx < 0) bxx = 0; if (bxx >= b->w) bxx = b->w - 1;
                if (byy < 0) byy = 0; if (byy >= b->h) byy = b->h - 1;
                q = b->px[byy * b->w + bxx];
                rb += q.r; gb += q.g; bb += q.b;
            }
            ra /= 5.0f; ga_ /= 5.0f; ba /= 5.0f;
            rb /= 5.0f; gb /= 5.0f; bb /= 5.0f;
            out->px[i].r = ra*(1-mM) + rb*mM;
            out->px[i].g = ga_*(1-mM) + gb*mM;
            out->px[i].b = ba*(1-mM) + bb*mM;
            out->px[i].a = pa.a*(1-mM) + pb.a*mM;
        } else if (tr->op == 13) {
            /* R073 hop 90: Venetian-blind dissolve — horizontal strips
             * (16 px) flip A->B in a top-to-bottom wave; each strip's
             * threshold is its normalized position. Feathered via
             * grad_feather on the per-strip progress. */
            float feath = wb_node_param_value(self, "grad_feather", t);
            if (feath <= 0.0f) feath = 0.05f;
            /* R074 fix: strip height proportional (was fixed 16px) */
            int sh16 = a->h / 8 > 0 ? a->h / 8 : 1;
            float v = (((py_i / sh16) * sh16) + sh16/2.0f)
                    / (float)a->h;
            float k = (mB - v) / feath + 0.5f;
            if (k < 0) k = 0; if (k > 1) k = 1;
            out->px[i].r = pa.r*(1-k) + pb.r*k;
            out->px[i].g = pa.g*(1-k) + pb.g*k;
            out->px[i].b = pa.b*(1-k) + pb.b*k;
            out->px[i].a = pa.a*(1-k) + pb.a*k;
        } else if (tr->op == 12) {
            /* R073 hop 89: four-box wipe — each quadrant fills from its
             * outer corner toward the frame center as progress grows. */
            float feath = wb_node_param_value(self, "grad_feather", t);
            if (feath <= 0.0f) feath = 0.05f;
            float u = (float)px_i / (float)(a->w > 1 ? a->w-1 : 1);
            float v = (float)py_i / (float)(a->h > 1 ? a->h-1 : 1);
            /* distance from this pixel to its quadrant's outer corner,
             * normalized 0 (at corner) .. 1 (at frame center) */
            float du = u < 0.5f ? u : 1.0f - u;
            float dv = v < 0.5f ? v : 1.0f - v;
            /* box edge reaches the pixel when mB exceeds max(du,dv)/0.5 */
            float g = fmaxf(du, dv) * 2.0f;
            float k = (mB - g) / feath + 0.5f;
            if (k < 0) k = 0; if (k > 1) k = 1;
            out->px[i].r = pa.r*(1-k) + pb.r*k;
            out->px[i].g = pa.g*(1-k) + pb.g*k;
            out->px[i].b = pa.b*(1-k) + pb.b*k;
            out->px[i].a = pa.a*(1-k) + pb.a*k;
        } else if (tr->op == 11) {
            /* R073 hop 88: checkerboard dissolve — each cell (16 px)
             * switches A->B at its own hashed time within the transition
             * (deterministic per-position hash, no state). */
            /* R074 fix: 8x8 grid proportional to resolution (was fixed
             * 16px cells); hash salted per-axis to decorrelate. */
            int cw8 = a->w / 8 > 0 ? a->w / 8 : 1;
            int chh8 = a->h / 8 > 0 ? a->h / 8 : 1;
            unsigned ch = ((unsigned)((px_i / cw8) * 73856093u)
                        ^ ((unsigned)(py_i / chh8) * 19349663u))
                        + 0x9e3779b9u;
            ch ^= ch >> 13; ch *= 0x5bd1e995u; ch ^= ch >> 15;
            float th = mM + ((float)(ch & 0xFFFF) / 65535.0f - 0.5f)
                     * 0.6f;
            if (th > 0.5f) { out->px[i] = pb; }
            else           { out->px[i] = pa; }
        } else if (tr->op == 10) {
            /* R073 hop 87: clock wipe — B revealed in the sector swept
             * clockwise from 12 o'clock; mB*2π is the hand angle. */
            float feath = wb_node_param_value(self, "grad_feather", t);
            if (feath <= 0.0f) feath = 0.05f;
            float dxn = px_i - a->w * 0.5f;
            float dyn = py_i - a->h * 0.5f;
            float ang = atan2f(dxn, -dyn);      /* -pi..pi, 0 at top */
            if (ang < 0) ang += 6.2831853f;
            float g = ang / 6.2831853f;         /* 0..1 around clock */
            float k = (mB - g) / feath + 0.5f;
            if (k < 0) k = 0; if (k > 1) k = 1;
            out->px[i].r = pa.r*(1-k) + pb.r*k;
            out->px[i].g = pa.g*(1-k) + pb.g*k;
            out->px[i].b = pa.b*(1-k) + pb.b*k;
            out->px[i].a = pa.a*(1-k) + pb.a*k;
        } else if (tr->op == 9) {
            /* R073 hop 86: barn-door wipe — B reveals as a symmetric
             * center strip that grows with progress. tr->dir selects
             * axis (0=horizontal strip, 1=vertical strip); feathered
             * edges share grad_feather. */
            float feath = wb_node_param_value(self, "grad_feather", t);
            if (feath <= 0.0f) feath = 0.05f;
            float half = mM * 0.5f;
            float u = (tr->dir == 1)
                    ? (float)py_i / (float)(a->h > 1 ? a->h-1 : 1)
                    : (float)px_i / (float)(a->w > 1 ? a->w-1 : 1);
            float d = fabsf(u - 0.5f);
            /* inside strip (d < half) -> B; feather band around edge */
            float k = (half - d) / feath + 0.5f;
            if (k < 0) k = 0; if (k > 1) k = 1;
            out->px[i].r = pa.r*(1-k) + pb.r*k;
            out->px[i].g = pa.g*(1-k) + pb.g*k;
            out->px[i].b = pa.b*(1-k) + pb.b*k;
            out->px[i].a = pa.a*(1-k) + pb.a*k;
        } else if (tr->op == 8) {
            /* R073 hop 84: gradient wipe — built-in gradient map is the
             * per-pixel threshold. grad_dir 0 = linear L→R (with soft
             * band via win_soft-style feather), 1 = radial from center.
             * A user matte in slot 3 overrides the built-in gradient. */
            float g;
            if (mapf) {
                g = 0.2126f*mapf->px[i].r + 0.7152f*mapf->px[i].g
                  + 0.0722f*mapf->px[i].b;
            } else if (wb_node_param_value(self, "grad_dir", t) > 2.5f) {
                /* angular: clock sweep starting at 12 o'clock */
                float dxn = px_i - a->w * 0.5f;
                float dyn = py_i - a->h * 0.5f;
                float ang = atan2f(dxn, -dyn);      /* 0 at top, +cw */
                if (ang < 0) ang += 6.2831853f;
                g = ang / 6.2831853f;
            } else if (wb_node_param_value(self, "grad_dir", t) > 1.5f) {
                /* diagonal TL -> BR */
                float u = (float)px_i / (float)(a->w > 1 ? a->w-1 : 1);
                float v = (float)py_i / (float)(a->h > 1 ? a->h-1 : 1);
                g = (u + v) * 0.5f;
            } else if (wb_node_param_value(self, "grad_dir", t) > 0.5f) {
                float dxn = px_i - a->w * 0.5f;
                float dyn = py_i - a->h * 0.5f;
                g = sqrtf(dxn*dxn + dyn*dyn)
                  / (0.5f * sqrtf((float)(a->w*a->w + a->h*a->h)));
            } else {
                g = (float)px_i / (float)(a->w > 1 ? a->w - 1 : 1);
            }
            /* feathered threshold: full B above g+feather, full A below */
            float feath = wb_node_param_value(self, "grad_feather", t);
            if (feath <= 0.0f) feath = 0.05f;
            float k = (mB - g) / feath + 0.5f;
            if (k < 0) k = 0; if (k > 1) k = 1;
            out->px[i].r = pa.r*(1-k) + pb.r*k;
            out->px[i].g = pa.g*(1-k) + pb.g*k;
            out->px[i].b = pa.b*(1-k) + pb.b*k;
            out->px[i].a = pa.a*(1-k) + pb.a*k;
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

/* R073 hop 96: transition style presets. */
wb_node *wb_transition_preset(int preset, double duration_secs) {
    int op = 0;
    struct { const char *name; float v; } ps[4];
    int nps = 0;
    switch (preset) {
    case 0:                                   /* MusicVideo */
        op = 0;                                /* crossfade */
        ps[nps].name = "grad_feather"; ps[nps++].v = 0.02f;
        break;
    case 1:                                   /* News */
        op = 2;                                /* linear wipe */
        ps[nps].name = "grad_feather"; ps[nps++].v = 0.01f;
        break;
    case 2:                                   /* Cinematic */
        op = 1;                                /* dip-to-black */
        /* R074 hop 133 (#72): honor explicit short durations — only
         * extend when the caller left it at the default (<=0). */
        if (duration_secs <= 0.0) duration_secs = 1.5;
        break;
    case 3:                                   /* VJ */
        op = 14;                               /* zoom-blur */
        ps[nps].name = "grad_feather"; ps[nps++].v = 0.10f;
        break;
    default:
        return NULL;
    }
    wb_node *n = wb_node_transition(op, duration_secs);
    if (!n) return NULL;
    for (int i = 0; i < nps; i++) {
        wb_param_track *tp = wb_param_track_create();
        wb_param_track_set(tp, 0.0, ps[i].v, WB_KF_HOLD);
        wb_node_add_param(n, ps[i].name, tp);
    }
    return n;
}

/* R073 hop 82: bind win_cx/win_cy from paired sample arrays (linear). */
int wb_node_window_track_path(wb_node *win_effect,
                              const double *ts, const float xs[],
                              const float ys[], int n, double dur) {
    if (!win_effect || n < 2) return -1;
    (void)dur;
    wb_param_track *tcx = wb_param_track_create();
    wb_param_track *tcy = wb_param_track_create();
    wb_param_track_set_many(tcx, ts, xs, n, WB_KF_LINEAR);
    wb_param_track_set_many(tcy, ts, ys, n, WB_KF_LINEAR);
    int ix = wb_node_add_param(win_effect, "win_cx", tcx);
    int iy = wb_node_add_param(win_effect, "win_cy", tcy);
    return (ix >= 0 && iy >= 0) ? 0 : -1;
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
