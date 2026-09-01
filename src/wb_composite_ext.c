/* wb_composite_ext.c — Advanced compositing nodes for After Effects parity
 * R088: adjustment layers, track mattes, frame blending
 *
 * Adjustment layer: a node that applies its input as an effect on everything
 * below it in the compositing stack. In node graph terms: a composite node
 * that takes a "source" input (the layers below) and an "effect" input
 * (the adjustment effect), and composites the effect over the source.
 *
 * Track matte: uses one layer's alpha channel as a mask for another layer.
 * In node graph terms: a composite node with 3 inputs — source, matte, and
 * a mode selector (alpha matte, luma matte, alpha inverted, luma inverted).
 *
 * Frame blending: for time remapping/slow-motion. Blends adjacent frames
 * to create smooth slow-motion. In node graph terms: a cache node that
 * interpolates between frames at fractional time positions.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_compositor.h"

/* ---- Track Matte ---- */

typedef struct {
    wb_node base;
    int matte_mode; /* 0=alpha matte, 1=luma matte, 2=alpha inverted, 3=luma inverted */
} wb_node_trackmatte;

wb_node *wb_node_trackmatte_create(void) {
    wb_node_trackmatte *n = (wb_node_trackmatte *)calloc(1, sizeof(wb_node_trackmatte));
    if (!n) return NULL;
    n->base.kind = WB_NODE_COMPOSITE;
    n->base.n_inputs = 2; /* input 0 = source, input 1 = matte */
    n->matte_mode = 0;
    return (wb_node *)n;
}

void wb_node_trackmatte_set_mode(wb_node *node, int mode) {
    if (!node || node->kind != WB_NODE_COMPOSITE) return;
    wb_node_trackmatte *n = (wb_node_trackmatte *)node;
    if (mode < 0) mode = 0;
    if (mode > 3) mode = 3;
    n->matte_mode = mode;
}

/* Pull track matte: use matte input's alpha/luma to mask source input */
static wb_frame *trackmatte_pull(wb_node *node, double t, int rx, int ry, int rw, int rh) {
    wb_node_trackmatte *tm = (wb_node_trackmatte *)node;

    /* Pull source and matte */
    wb_frame *src_frame = NULL, *matte_frame = NULL;

    if (node->inputs[0]) src_frame = wb_node_pull(node->inputs[0], t, rx, ry, rw, rh);
    if (node->inputs[1]) matte_frame = wb_node_pull(node->inputs[1], t, rx, ry, rw, rh);

    if (!src_frame) return matte_frame;
    if (!matte_frame) return src_frame;

    /* Apply matte */
    int count = rw * rh;
    for (int i = 0; i < count; i++) {
        float matte_val = 0;
        float ma = matte_frame->px[i].a;

        switch (tm->matte_mode) {
        case 0: /* Alpha matte */
            matte_val = ma;
            break;
        case 1: /* Luma matte */
            matte_val = matte_frame->px[i].r * 0.299f +
                        matte_frame->px[i].g * 0.587f +
                        matte_frame->px[i].b * 0.114f;
            break;
        case 2: /* Alpha inverted */
            matte_val = 1.0f - ma;
            break;
        case 3: /* Luma inverted */
            matte_val = 1.0f - (matte_frame->px[i].r * 0.299f +
                                 matte_frame->px[i].g * 0.587f +
                                 matte_frame->px[i].b * 0.114f);
            break;
        }

        src_frame->px[i].a *= matte_val;
    }

    wb_frame_free(matte_frame);
    return src_frame;
}

/* ---- Frame Blending ---- */

typedef struct {
    wb_node base;
    float blend_factor; /* 0.0 = frame N, 1.0 = frame N+1 */
} wb_node_frameblend;

wb_node *wb_node_frameblend_create(void) {
    wb_node_frameblend *n = (wb_node_frameblend *)calloc(1, sizeof(wb_node_frameblend));
    if (!n) return NULL;
    n->base.kind = WB_NODE_CACHE;
    n->base.n_inputs = 1;
    n->blend_factor = 0.5f;
    return (wb_node *)n;
}

void wb_node_frameblend_set_factor(wb_node *node, float factor) {
    if (!node) return;
    wb_node_frameblend *n = (wb_node_frameblend *)node;
    n->blend_factor = fminf(1.0f, fmaxf(0.0f, factor));
}

/* Pull frame blend: interpolate between two time positions */
static wb_frame *frameblend_pull(wb_node *node, double t, int rx, int ry, int rw, int rh) {
    wb_node_frameblend *fb = (wb_node_frameblend *)node;
    if (!node->inputs[0]) return NULL;

    /* Pull at current time and next frame time */
    double fps = 30.0; /* default, could be from graph */
    double dt = 1.0 / fps;

    wb_frame *frame_a = wb_node_pull(node->inputs[0], t, rx, ry, rw, rh);
    wb_frame *frame_b = wb_node_pull(node->inputs[0], t + dt, rx, ry, rw, rh);

    if (!frame_a) return frame_b;
    if (!frame_b) return frame_a;

    /* Blend */
    float bf = fb->blend_factor;
    int count = rw * rh;
    for (int i = 0; i < count; i++) {
        frame_a->px[i].r = frame_a->px[i].r * (1.0f - bf) + frame_b->px[i].r * bf;
        frame_a->px[i].g = frame_a->px[i].g * (1.0f - bf) + frame_b->px[i].g * bf;
        frame_a->px[i].b = frame_a->px[i].b * (1.0f - bf) + frame_b->px[i].b * bf;
        frame_a->px[i].a = frame_a->px[i].a * (1.0f - bf) + frame_b->px[i].a * bf;
    }

    wb_frame_free(frame_b);
    return frame_a;
}

/* ---- Adjustment Layer ---- */

typedef struct {
    wb_node base;
    wb_node *effect_chain[8]; /* up to 8 effects to apply */
    int effect_count;
} wb_node_adjustment;

wb_node *wb_node_adjustment_create(void) {
    wb_node_adjustment *n = (wb_node_adjustment *)calloc(1, sizeof(wb_node_adjustment));
    if (!n) return NULL;
    n->base.kind = WB_NODE_COMPOSITE;
    n->base.n_inputs = 2; /* input 0 = source (layers below), input 1 = effect mask (optional) */
    n->effect_count = 0;
    return (wb_node *)n;
}

int wb_node_adjustment_add_effect(wb_node *node, wb_node *effect) {
    if (!node || !effect) return -1;
    wb_node_adjustment *n = (wb_node_adjustment *)node;
    if (n->effect_count >= 8) return -1;
    n->effect_chain[n->effect_count++] = effect;
    return 0;
}

/* Pull adjustment: apply effect chain to source */
static wb_frame *adjustment_pull(wb_node *node, double t, int rx, int ry, int rw, int rh) {
    wb_node_adjustment *adj = (wb_node_adjustment *)node;
    if (!node->inputs[0]) return NULL;

    /* Pull source (everything below) */
    wb_frame *src = wb_node_pull(node->inputs[0], t, rx, ry, rw, rh);
    if (!src) return NULL;

    /* Apply each effect in the chain */
    for (int i = 0; i < adj->effect_count; i++) {
        wb_node *fx = adj->effect_chain[i];
        if (!fx) continue;

        /* Set the effect's input to our source frame */
        if (fx->n_inputs > 0 && fx->inputs) {
            fx->inputs[0] = NULL; /* Will be pulled from src below */
        }

        /* Pull effect — it processes the frame */
        wb_frame *fx_out = wb_node_pull(fx, t, rx, ry, rw, rh);
        if (fx_out && fx_out != src) {
            /* Copy result back to src */
            int count = rw * rh;
            for (int p = 0; p < count; p++) {
                src->px[p] = fx_out->px[p];
            }
            wb_frame_free(fx_out);
        }
    }

    return src;
}
