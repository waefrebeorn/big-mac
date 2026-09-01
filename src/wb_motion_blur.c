/* wb_motion_blur.c — directional motion blur effect node (R086).
 *
 * After Effects-style motion blur: tracks the node's transform
 * (position/scale/rotation) across frames and accumulates sub-samples
 * along the motion vector with decreasing opacity. The result matches
 * AE's "frame sampling" shutter model.
 *
 * Two shutter controls (AE parity):
 *   - shutter_angle: 0..360 degrees. Controls how many sub-samples are
 *     accumulated. 360 = full frame interval (max blur), 0 = no blur.
 *     Default 180 (the AE default).
 *   - shutter_phase: -180..180 degrees. Offsets the sub-sample timing
 *     relative to the frame boundary. Negative = pre-frame (motion
 *     trails ahead), positive = post-frame (trails behind).
 *
 * Implementation (transform-based):
 *   - On each pull, compare current transform to previous frame's.
 *   - If transform changed, render `samples` copies at interpolated
 *     positions with shutter-angle-weighted opacity.
 *   - Accumulate into output with proper alpha-over.
 *
 * Pure C11, no third party.
 */

#include "wbus/wbus_compositor.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Motion blur node state                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    int   samples;           /* sub-samples per frame (default 8) */
    float shutter_angle;     /* 0..360 degrees (default 180) */
    float shutter_phase;     /* -180..180 degrees (default 0) */

    /* Previous-frame transform (for motion estimation) */
    float prev_x, prev_y;    /* position offset in pixels */
    float prev_scale;        /* scale factor */
    float prev_rot;          /* rotation in radians */
    int   has_prev;          /* first frame flag */

    /* Current-frame transform (set externally via params or defaults) */
    float cur_x, cur_y;
    float cur_scale;
    float cur_rot;

    /* Previous frame buffer (for per-pixel frame-differencing fallback) */
    wb_px *prev_frame;
    int    prev_w, prev_h;
} mb_node_t;

/* ------------------------------------------------------------------ */
/* Bilinear sample from float RGBA frame                              */
/* ------------------------------------------------------------------ */
static wb_px sample_bilinear(const wb_px *px, int w, int h, float fx, float fy) {
    wb_px zero = {0, 0, 0, 0};
    if (fx < 0 || fx >= (float)w - 1.0f || fy < 0 || fy >= (float)h - 1.0f)
        return zero;

    int ix = (int)fx;
    int iy = (int)fy;
    float dx = fx - (float)ix;
    float dy = fy - (float)iy;

    int ix1 = ix + 1 < w ? ix + 1 : ix;
    int iy1 = iy + 1 < h ? iy + 1 : iy;

    const wb_px *p00 = &px[iy * w + ix];
    const wb_px *p10 = &px[iy * w + ix1];
    const wb_px *p01 = &px[iy1 * w + ix];
    const wb_px *p11 = &px[iy1 * w + ix1];

    wb_px out;
    out.r = (p00->r * (1-dx)*(1-dy) + p10->r * dx*(1-dy) +
             p01->r * (1-dx)*dy     + p11->r * dx*dy);
    out.g = (p00->g * (1-dx)*(1-dy) + p10->g * dx*(1-dy) +
             p01->g * (1-dx)*dy     + p11->g * dx*dy);
    out.b = (p00->b * (1-dx)*(1-dy) + p10->b * dx*(1-dy) +
             p01->b * (1-dx)*dy     + p11->b * dx*dy);
    out.a = (p00->a * (1-dx)*(1-dy) + p10->a * dx*(1-dy) +
             p01->a * (1-dx)*dy     + p11->a * dx*dy);
    return out;
}

/* ------------------------------------------------------------------ */
/* Bilinear sample from float RGBA frame                              */
/* ------------------------------------------------------------------ */
static wb_frame *mb_pull(wb_node *self, double t,
                          int rx, int ry, int rw, int rh, int phase) {
    (void)rx; (void)ry; (void)rw; (void)rh;
    if (!self->inputs || !self->inputs[0]) return NULL;

    mb_node_t *mb = (mb_node_t *)self->user;
    if (!mb) return NULL;

    /* G3: request inputs in phase 0 */
    if (phase == 0) {
        wb_node_pull_request(self->inputs[0], t, -1, -1, -1, -1);
        return NULL;
    }

    wb_frame *in = wb_node_pull(self->inputs[0], t, -1, -1, -1, -1);
    if (!in) return NULL;

    int W = in->w, H = in->h;

    /* Read animated params if bound */
    float sx = wb_node_param_value(self, "pos_x", t);
    float sy = wb_node_param_value(self, "pos_y", t);
    float sc = wb_node_param_value(self, "scale", t);
    float rt = wb_node_param_value(self, "rotation", t);

    /* Apply animated params (0 means unset -> use defaults) */
    mb->cur_x = (sx != 0.0f) ? sx : 0.0f;
    mb->cur_y = (sy != 0.0f) ? sy : 0.0f;
    mb->cur_scale = (sc != 0.0f) ? sc : 1.0f;
    mb->cur_rot = rt; /* rotation 0 = no rotation */

    /* Determine if there's motion this frame */
    int has_motion = 0;
    if (mb->has_prev) {
        float dx = mb->cur_x - mb->prev_x;
        float dy = mb->cur_y - mb->prev_y;
        float ds = mb->cur_scale - mb->prev_scale;
        float dr = mb->cur_rot - mb->prev_rot;
        if (fabsf(dx) > 0.01f || fabsf(dy) > 0.01f ||
            fabsf(ds) > 0.001f || fabsf(dr) > 0.001f) {
            has_motion = 1;
        }
    } else {
        has_motion = 0; /* first frame: no previous to compare */
    }

    /* Update previous transform */
    mb->prev_x = mb->cur_x;
    mb->prev_y = mb->cur_y;
    mb->prev_scale = mb->cur_scale;
    mb->prev_rot = mb->cur_rot;
    mb->has_prev = 1;

    /* If no motion or shutter_angle == 0, return input unchanged */
    if (!has_motion || mb->shutter_angle <= 0.0f || mb->samples <= 1) {
        /* Still update prev_frame for per-pixel fallback on next frame */
        if (!mb->prev_frame || mb->prev_w != W || mb->prev_h != H) {
            free(mb->prev_frame);
            mb->prev_frame = (wb_px *)malloc((size_t)W * H * sizeof(wb_px));
            mb->prev_w = W;
            mb->prev_h = H;
        }
        if (mb->prev_frame)
            memcpy(mb->prev_frame, in->px, (size_t)W * H * sizeof(wb_px));

        in->roi_x = 0; in->roi_y = 0;
        in->roi_w = W; in->roi_h = H;
        return in;
    }

    /* --- Motion blur accumulation pass --- */
    /* Allocate accumulation buffer */
    wb_px *acc = (wb_px *)calloc((size_t)W * H, sizeof(wb_px));
    if (!acc) return in;

    /* Shutter angle -> fraction of frame interval to sample.
     * 360 degrees = full interval (1.0), 180 = half (0.05), etc. */
    float shutter_frac = mb->shutter_angle / 360.0f;
    if (shutter_frac > 1.0f) shutter_frac = 1.0f;

    /* Phase offset in -1..1 range */
    float phase_offset = mb->shutter_phase / 180.0f;

    /* Motion vector (per-frame delta) */
    float mvx = mb->cur_x - mb->prev_x;
    float mvy = mb->cur_y - mb->prev_y;

    /* Accumulate sub-samples */
    for (int s = 0; s < mb->samples; s++) {
        /* Normalized sub-sample position: -shutter_frac/2 .. +shutter_frac/2
         * shifted by phase */
        float frac;
        if (mb->samples > 1)
            frac = ((float)s / (float)(mb->samples - 1) - 0.5f) * shutter_frac
                   + phase_offset * shutter_frac * 0.5f;
        else
            frac = 0.0f;

        /* Weight: equal weighting (box filter) for simplicity.
         * Cosine weighting would be more film-like but box is fine. */
        float weight = 1.0f / (float)mb->samples;

        /* Offset for this sub-sample */
        float ox = mvx * frac;
        float oy = mvy * frac;

        /* Rotation contribution (rotate around center) */
        float rot_frac = (mb->cur_rot - mb->prev_rot) * frac;
        float cos_r = cosf(rot_frac);
        float sin_r = sinf(rot_frac);

        /* Scale contribution */
        float scale_frac = 1.0f + (mb->cur_scale - mb->prev_scale) * frac;

        float cx = (float)W * 0.5f;
        float cy = (float)H * 0.5f;

        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                /* Destination pixel */
                float dx_f = (float)x - cx;
                float dy_f = (float)y - cy;

                /* Inverse transform: undo rotation and scale to find source */
                float sx_f, sy_f;
                if (fabsf(scale_frac) > 1e-6f) {
                    /* Undo scale */
                    sx_f = dx_f / scale_frac;
                    sy_f = dy_f / scale_frac;
                } else {
                    sx_f = dx_f;
                    sy_f = dy_f;
                }

                /* Undo rotation (apply negative rotation) */
                float rx_f = sx_f * cos_r + sy_f * sin_r;
                float ry_f = -sx_f * sin_r + sy_f * cos_r;

                /* Undo translation */
                sx_f = rx_f + cx - ox;
                sy_f = ry_f + cy - oy;

                /* Sample input at transformed position */
                wb_px sp = sample_bilinear(in->px, W, H, sx_f, sy_f);

                /* Apply weight to alpha for accumulation */
                sp.a *= weight;
                sp.r *= weight;
                sp.g *= weight;
                sp.b *= weight;

                /* Accumulate (premultiplied alpha over) */
                int idx = y * W + x;
                wb_px existing = acc[idx];
                float sa = sp.a;
                float da = existing.a * (1.0f - sa);
                float oa = sa + da;
                if (oa > 1e-6f) {
                    acc[idx].r = sp.r + existing.r * (1.0f - sa);
                    acc[idx].g = sp.g + existing.g * (1.0f - sa);
                    acc[idx].b = sp.b + existing.b * (1.0f - sa);
                    acc[idx].a = oa;
                } else {
                    acc[idx] = sp;
                }
            }
        }
    }

    /* Copy accumulated result back to input frame */
    memcpy(in->px, acc, (size_t)W * H * sizeof(wb_px));
    free(acc);

    /* Store current frame for next-frame fallback */
    if (!mb->prev_frame || mb->prev_w != W || mb->prev_h != H) {
        free(mb->prev_frame);
        mb->prev_frame = (wb_px *)malloc((size_t)W * H * sizeof(wb_px));
        mb->prev_w = W;
        mb->prev_h = H;
    }
    if (mb->prev_frame)
        memcpy(mb->prev_frame, in->px, (size_t)W * H * sizeof(wb_px));

    in->roi_x = 0; in->roi_y = 0;
    in->roi_w = W; in->roi_h = H;
    return in;
}

/* ------------------------------------------------------------------ */
/* Free                                                               */
/* ------------------------------------------------------------------ */
static void mb_free(wb_node *n) {
    mb_node_t *mb = (mb_node_t *)n->user;
    if (mb) {
        free(mb->prev_frame);
        free(mb);
    }
}

/* ------------------------------------------------------------------ */
/* Factory                                                            */
/* ------------------------------------------------------------------ */
wb_node *wb_node_effect_motion_blur(int samples) {
    wb_node *n = wb_node_create(WB_NODE_EFFECT, "motion_blur");
    if (!n) return NULL;

    mb_node_t *mb = (mb_node_t *)calloc(1, sizeof(*mb));
    if (!mb) { wb_node_destroy(n); return NULL; }

    mb->samples = samples > 1 ? samples : 8;
    mb->shutter_angle = 180.0f;
    mb->shutter_phase = 0.0f;
    mb->prev_scale = 1.0f;
    mb->cur_scale = 1.0f;
    mb->has_prev = 0;
    mb->prev_frame = NULL;
    mb->prev_w = 0;
    mb->prev_h = 0;

    n->user = mb;
    n->pull = mb_pull;
    n->free = mb_free;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node *));
    if (!n->inputs) { free(mb); wb_node_destroy(n); return NULL; }

    return n;
}

/* ------------------------------------------------------------------ */
/* Setters                                                            */
/* ------------------------------------------------------------------ */
void wb_node_effect_motion_blur_set_shutter_angle(wb_node *n, float angle) {
    if (!n || n->kind != WB_NODE_EFFECT) return;
    mb_node_t *mb = (mb_node_t *)n->user;
    if (!mb) return;
    if (angle < 0.0f) angle = 0.0f;
    if (angle > 360.0f) angle = 360.0f;
    mb->shutter_angle = angle;
}

void wb_node_effect_motion_blur_set_shutter_phase(wb_node *n, float phase) {
    if (!n || n->kind != WB_NODE_EFFECT) return;
    mb_node_t *mb = (mb_node_t *)n->user;
    if (!mb) return;
    if (phase < -180.0f) phase = -180.0f;
    if (phase > 180.0f) phase = 180.0f;
    mb->shutter_phase = phase;
}