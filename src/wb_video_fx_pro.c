/* wb_video_fx_pro.c — professional video FX nodes (R085).
 *
 * Wraps existing modules as compositor effect nodes:
 *   - Stabilization (wb_stabilize2) — smooths camera shake
 *   - Chroma key (wb_chromakey) — green screen compositing
 *   - Motion tracking data source (wb_motion_track) — drives transform params
 *
 * These are single-input effect nodes that operate on RGBA frames.
 */

#include "wbus/wbus_compositor.h"
#include "wbus/wbus_vfx.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- Stabilization node ---------------------------------------------- */

void *wb_stabilize2_create(int width, int height);
void wb_stabilize2_destroy(void *inst);
int wb_stabilize2_process(void *inst, uint8_t *frame_rgba, int width, int height);
void wb_stabilize2_reset(void *inst);

typedef struct {
    void *stabilizer;
    int w, h;
    int enabled;
} stab_node_t;

static wb_frame *stab_pull(wb_node *self, double t,
                            int rx, int ry, int rw, int rh, int phase) {
    (void)t;
    stab_node_t *s = self->user;
    if (self->n_inputs < 1) return NULL;
    if (phase == 0) {
        wb_node_pull_request(self->inputs[0], t, rx, ry, rw, rh);
        return NULL;
    }
    wb_frame *in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
    if (!in || !s->enabled) return in;

    /* Convert float to uint8, stabilize, convert back */
    int w = in->w, h = in->h;
    uint8_t *rgba = malloc(w * h * 4);
    for (int i = 0; i < w * h; i++) {
        rgba[i*4+0] = (uint8_t)(in->px[i].r * 255.0f + 0.5f);
        rgba[i*4+1] = (uint8_t)(in->px[i].g * 255.0f + 0.5f);
        rgba[i*4+2] = (uint8_t)(in->px[i].b * 255.0f + 0.5f);
        rgba[i*4+3] = (uint8_t)(in->px[i].a * 255.0f + 0.5f);
    }

    wb_stabilize2_process(s->stabilizer, rgba, w, h);

    for (int i = 0; i < w * h; i++) {
        in->px[i].r = rgba[i*4+0] / 255.0f;
        in->px[i].g = rgba[i*4+1] / 255.0f;
        in->px[i].b = rgba[i*4+2] / 255.0f;
        in->px[i].a = rgba[i*4+3] / 255.0f;
    }
    free(rgba);
    return in;
}

static void stab_free(wb_node *n) {
    stab_node_t *s = n->user;
    if (s->stabilizer) wb_stabilize2_destroy(s->stabilizer);
    free(s);
}

wb_node *wb_node_effect_stabilize(void) {
    wb_node *n = wb_node_create(WB_NODE_EFFECT, "stabilize");
    if (!n) return NULL;
    stab_node_t *s = calloc(1, sizeof(*s));
    if (!s) { wb_node_destroy(n); return NULL; }
    s->w = 854; s->h = 480;
    s->stabilizer = wb_stabilize2_create(s->w, s->h);
    s->enabled = 1;
    n->user = s;
    n->pull = stab_pull;
    n->free = stab_free;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node *));
    return n;
}

void wb_node_effect_stabilize_set_enabled(wb_node *n, int enabled) {
    if (!n || n->kind != WB_NODE_EFFECT) return;
    stab_node_t *s = n->user;
    if (s) s->enabled = enabled;
}

/* ---- Chroma key node -------------------------------------------------- */

void *wb_chromakey_create(int width, int height);
void wb_chromakey_destroy(void *inst);
void wb_chromakey_set_key_color(void *inst, float r, float g, float b);
void wb_chromakey_set_threshold(void *inst, float t);
void wb_chromakey_set_softness(void *inst, float s);
void wb_chromakey_process(void *inst, const uint8_t *fg, uint8_t *out, int w, int h);

typedef struct {
    void *keyer;
    int w, h;
    float key_r, key_g, key_b;
    float threshold;
    float softness;
} ck_node_t;

static wb_frame *ck_pull(wb_node *self, double t,
                          int rx, int ry, int rw, int rh, int phase) {
    (void)t;
    ck_node_t *k = self->user;
    if (self->n_inputs < 1) return NULL;
    if (phase == 0) {
        wb_node_pull_request(self->inputs[0], t, rx, ry, rw, rh);
        return NULL;
    }
    wb_frame *in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
    if (!in) return NULL;

    int w = in->w, h = in->h;
    uint8_t *rgba = malloc(w * h * 4);
    uint8_t *out = malloc(w * h * 4);
    for (int i = 0; i < w * h; i++) {
        rgba[i*4+0] = (uint8_t)(in->px[i].r * 255.0f + 0.5f);
        rgba[i*4+1] = (uint8_t)(in->px[i].g * 255.0f + 0.5f);
        rgba[i*4+2] = (uint8_t)(in->px[i].b * 255.0f + 0.5f);
        rgba[i*4+3] = (uint8_t)(in->px[i].a * 255.0f + 0.5f);
    }

    wb_chromakey_process(k->keyer, rgba, out, w, h);

    for (int i = 0; i < w * h; i++) {
        in->px[i].r = out[i*4+0] / 255.0f;
        in->px[i].g = out[i*4+1] / 255.0f;
        in->px[i].b = out[i*4+2] / 255.0f;
        in->px[i].a = out[i*4+3] / 255.0f;
    }
    free(rgba);
    free(out);
    return in;
}

static void ck_free(wb_node *n) {
    ck_node_t *k = n->user;
    if (k->keyer) wb_chromakey_destroy(k->keyer);
    free(k);
}

wb_node *wb_node_effect_chromakey(float r, float g, float b, float threshold) {
    wb_node *n = wb_node_create(WB_NODE_EFFECT, "chromakey");
    if (!n) return NULL;
    ck_node_t *k = calloc(1, sizeof(*k));
    if (!k) { wb_node_destroy(n); return NULL; }
    k->w = 854; k->h = 480;
    k->keyer = wb_chromakey_create(k->w, k->h);
    k->key_r = r; k->key_g = g; k->key_b = b;
    k->threshold = threshold > 0 ? threshold : 0.4f;
    k->softness = 0.1f;
    if (k->keyer) {
        wb_chromakey_set_key_color(k->keyer, r, g, b);
        wb_chromakey_set_threshold(k->keyer, k->threshold);
        wb_chromakey_set_softness(k->keyer, k->softness);
    }
    n->user = k;
    n->pull = ck_pull;
    n->free = ck_free;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node *));
    return n;
}

void wb_node_effect_chromakey_set_color(wb_node *n, float r, float g, float b) {
    if (!n || n->kind != WB_NODE_EFFECT) return;
    ck_node_t *k = n->user;
    if (k && k->keyer) {
        k->key_r = r; k->key_g = g; k->key_b = b;
        wb_chromakey_set_key_color(k->keyer, r, g, b);
    }
}

/* ---- Transform node (position/scale/rotation from motion tracking) ---- */

typedef struct {
    float pos_x, pos_y;       /* Normalized position (0..1) */
    float scale;              /* 1.0 = normal */
    float rotation;           /* Radians */
    int track_data_available;
} transform_node_t;

static wb_frame *transform_pull(wb_node *self, double t,
                                 int rx, int ry, int rw, int rh, int phase) {
    transform_node_t *tr = self->user;
    if (self->n_inputs < 1) return NULL;
    if (phase == 0) {
        wb_node_pull_request(self->inputs[0], t, rx, ry, rw, rh);
        return NULL;
    }
    wb_frame *in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
    if (!in) return NULL;

    /* Apply transform: scale + translate + rotate */
    if (tr->scale == 1.0f && tr->pos_x == 0.5f && tr->pos_y == 0.5f && tr->rotation == 0.0f)
        return in;  /* Identity */

    int w = in->w, h = in->h;
    float cx = tr->pos_x * w;
    float cy = tr->pos_y * h;
    float cos_r = cosf(tr->rotation);
    float sin_r = sinf(tr->rotation);

    /* Create temp buffer */
    wb_px *tmp = calloc((size_t)w * h, sizeof(wb_px));
    memcpy(tmp, in->px, (size_t)w * h * sizeof(wb_px));

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            /* Inverse transform: destination -> source */
            float dx = x - cx;
            float dy = y - cy;
            /* Undo rotation */
            float rx2 = dx * cos_r + dy * sin_r;
            float ry2 = -dx * sin_r + dy * cos_r;
            /* Undo scale */
            float sx = rx2 / tr->scale + cx;
            float sy = ry2 / tr->scale + cy;

            /* Bilinear sample */
            int ix = (int)sx, iy = (int)sy;
            float fx = sx - ix, fy = sy - iy;
            if (ix < 0 || ix >= w - 1 || iy < 0 || iy >= h - 1) {
                in->px[y * w + x].a = 0.0f;  /* Transparent outside */
                continue;
            }
            wb_px *p00 = &tmp[iy * w + ix];
            wb_px *p10 = &tmp[iy * w + ix + 1];
            wb_px *p01 = &tmp[(iy + 1) * w + ix];
            wb_px *p11 = &tmp[(iy + 1) * w + ix + 1];
            wb_px *out_px = &in->px[y * w + x];
            out_px->r = p00->r * (1-fx)*(1-fy) + p10->r*fx*(1-fy) + p01->r*(1-fx)*fy + p11->r*fx*fy;
            out_px->g = p00->g * (1-fx)*(1-fy) + p10->g*fx*(1-fy) + p01->g*(1-fx)*fy + p11->g*fx*fy;
            out_px->b = p00->b * (1-fx)*(1-fy) + p10->b*fx*(1-fy) + p01->b*(1-fx)*fy + p11->b*fx*fy;
            out_px->a = p00->a * (1-fx)*(1-fy) + p10->a*fx*(1-fy) + p01->a*(1-fx)*fy + p11->a*fx*fy;
        }
    }

    free(tmp);
    return in;
}

static void transform_free(wb_node *n) { free(n->user); }

wb_node *wb_node_effect_transform_pro(void) {
    wb_node *n = wb_node_create(WB_NODE_EFFECT, "transform_pro");
    if (!n) return NULL;
    transform_node_t *tr = calloc(1, sizeof(*tr));
    if (!tr) { wb_node_destroy(n); return NULL; }
    tr->pos_x = 0.5f;
    tr->pos_y = 0.5f;
    tr->scale = 1.0f;
    tr->rotation = 0.0f;
    n->user = tr;
    n->pull = transform_pull;
    n->free = transform_free;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node *));
    return n;
}

void wb_node_effect_transform_pro_set_pos(wb_node *n, float x, float y) {
    if (!n || n->kind != WB_NODE_EFFECT) return;
    transform_node_t *tr = n->user;
    if (tr) { tr->pos_x = x; tr->pos_y = y; }
}

void wb_node_effect_transform_pro_set_scale(wb_node *n, float s) {
    if (!n || n->kind != WB_NODE_EFFECT) return;
    transform_node_t *tr = n->user;
    if (tr) tr->scale = s > 0.01f ? s : 0.01f;
}

void wb_node_effect_transform_pro_set_rotation(wb_node *n, float radians) {
    if (!n || n->kind != WB_NODE_EFFECT) return;
    transform_node_t *tr = n->user;
    if (tr) tr->rotation = radians;
}
