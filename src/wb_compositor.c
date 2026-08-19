/* wb_compositor.c — pull-based RoI/RoD node compositor (R013 D1/D3).
 * Pure C11. Recursive pull with identity skip + edge cache (LRU). */

#include "wbus/wbus_compositor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ---- G1: global quality-of-service dial (0..1) ----------------------- */
static double g_quality = 1.0;   /* default full quality */
static int    g_backend = WB_BACKEND_CPU;  /* G12: CPU authoritative */

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
void wb_compositor_set_backend(wb_backend b) {
    /* CPU path is authoritative; flipping to GPU only marks the boundary
     * where a Metal interop layer would wrap wb_px buffers. */
    g_backend = (b == WB_BACKEND_GPU) ? WB_BACKEND_GPU : WB_BACKEND_CPU;
}
wb_backend wb_compositor_get_backend(void) { return (wb_backend)g_backend; }
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
