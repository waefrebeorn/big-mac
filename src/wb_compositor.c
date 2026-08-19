/* wb_compositor.c — pull-based RoI/RoD node compositor (R013 D1/D3).
 * Pure C11. Recursive pull with identity skip + edge cache (LRU). */

#include "wbus/wbus_compositor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

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
    free(n);
}

/* ---- generic pull (identity short-circuit + forwarding) -------------- */
wb_frame *wb_node_pull(wb_node *n, double t, int rx, int ry, int rw, int rh) {
    if (!n) return NULL;
    if (!wb_roi_clip(4096, 4096, &rx, &ry, &rw, &rh)) return NULL;
    /* two-phase: phase 0 just requests inputs (used by caller pattern);
     * our nodes compute immediately, so phase is forwarded. */
    return n->pull(n, t, rx, ry, rw, rh, 1);
}

/* ---- SOURCE (color producer) ----------------------------------------- */
typedef struct { float r,g,b,a; int w,h; } src_color_t;
static wb_frame *src_color_pull(wb_node *self, double t,
                                int rx, int ry, int rw, int rh, int phase) {
    src_color_t *s = self->user;
    (void)t;
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

/* ---- EFFECT (gain / invert-alpha) ------------------------------------ */
typedef struct { int op; float gain; } eff_t;
static wb_frame *eff_pull(wb_node *self, double t,
                          int rx, int ry, int rw, int rh, int phase) {
    eff_t *e = self->user;
    if (self->n_inputs < 1) return NULL;
    /* identity shortcut: op 0 = bypass */
    if (e->op == 0) {
        wb_frame *in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
        return in;  /* pass through (VirtualDub-style bypass) */
    }
    wb_frame *in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
    if (!in) return NULL;
    in->roi_x = rx; in->roi_y = ry; in->roi_w = rw; in->roi_h = rh;
    for (int y = ry; y < ry + rh; y++)
        for (int x = rx; x < rx + rw; x++) {
            wb_px *p = &in->px[y*in->w + x];
            if (e->op == 1) { p->r*=e->gain; p->g*=e->gain; p->b*=e->gain; }
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
    uint64_t h = cache_hash(self->id, t, rx, ry, rw, rh);
    /* cache hit? */
    for (int i = 0; i < c->count; i++) {
        if (c->ents[i].hash == h) {
            c->ents[i].last = ++c->clock;   /* refresh LRU */
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

/* expose comp_add for tests via a small public wrapper */
void wb_composite_add(wb_node *comp, wb_node *child) { comp_add(comp, child); }
