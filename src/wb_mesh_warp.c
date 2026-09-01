/* wb_mesh_warp.c — mesh warp / puppet tool effect node (After Effects parity).
 *
 * Divides the input into a grid of (grid_w x grid_h) cells. Each grid
 * vertex can be pinned to an arbitrary position. When a pin is moved,
 * surrounding vertices follow based on distance and stiffness — the core
 * of AE's Puppet Tool for character deformation.
 *
 * Pull model: for each output pixel, find which cell it's in, compute the
 * deformed vertex positions, bilinearly interpolate to get the source
 * position in the input, then sample the input at that position.
 *
 * Pure C11, no third party.
 */

#include "wbus/wbus_compositor.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* Maximum grid size to bound memory usage */
#define WB_MESH_WARP_MAX_GRID 64

/* A pin: overrides the resting position of one grid vertex */
typedef struct {
    int active;       /* 0 = not pinned, 1 = pinned */
    float pin_x, pin_y;  /* pinned position in pixel space */
} wb_mesh_pin_t;

/* Mesh warp node state */
typedef struct {
    int grid_w, grid_h;     /* number of cells across/down */
    int vert_w, vert_h;     /* vertices = (grid_w+1) x (grid_h+1) */
    float stiffness;        /* 0.0-1.0, how far the deformation propagates */
    float *deform_x;        /* vert_w*vert_h deformed x positions (pixel space) */
    float *deform_y;        /* vert_w*vert_h deformed y positions (pixel space) */
    wb_mesh_pin_t *pins;    /* vert_w*vert_h pin slots */
} wb_mesh_warp_t;

/* ---- helpers ---------------------------------------------------------- */

/* Gaussian falloff: exp(-(d^2) / (2*sigma^2)), d in pixels */
static float gaussian_falloff(float d, float sigma) {
    if (sigma < 0.001f) return (d < 0.5f) ? 1.0f : 0.0f;
    float x = d / sigma;
    return expf(-0.5f * x * x);
}

/* Compute the resting (undeformed) position of a grid vertex */
static void resting_pos(const wb_mesh_warp_t *mw, int vx, int vy,
                        float img_w, float img_h, float *rx, float *ry) {
    /* grid vertex (vx, vy) maps to pixel position */
    *rx = (float)vx / (float)(mw->vert_w - 1) * (float)(img_w - 1);
    *ry = (float)vy / (float)(mw->vert_h - 1) * (float)(img_h - 1);
}

/* Recompute the deformed grid from pins.
 * For each vertex, accumulate weighted pin displacements using Gaussian
 * falloff. The influence radius is derived from stiffness:
 *   stiffness 0 -> only pinned vertices move (sigma ~ 1px)
 *   stiffness 1 -> whole mesh follows (sigma ~ image diagonal) */
static void mesh_warp_recompute(wb_mesh_warp_t *mw, int img_w, int img_h) {
    float diag = sqrtf((float)(img_w * img_w + img_h * img_h));
    /* sigma scales with stiffness: 0.01*diag at stiffness=0 -> diag at stiffness=1 */
    float sigma = diag * (0.01f + mw->stiffness * 0.99f);

    for (int vy = 0; vy < mw->vert_h; vy++) {
        for (int vx = 0; vx < mw->vert_w; vx++) {
            int vi = vy * mw->vert_w + vx;
            float rx, ry;
            resting_pos(mw, vx, vy, img_w, img_h, &rx, &ry);

            /* If this vertex itself is pinned, it goes exactly to the pin */
            if (mw->pins[vi].active) {
                mw->deform_x[vi] = mw->pins[vi].pin_x;
                mw->deform_y[vi] = mw->pins[vi].pin_y;
                continue;
            }

            /* Accumulate weighted pin displacements */
            float total_w = 0.0f;
            float disp_x = 0.0f;
            float disp_y = 0.0f;

            for (int py = 0; py < mw->vert_h; py++) {
                for (int px = 0; px < mw->vert_w; px++) {
                    int pi = py * mw->vert_w + px;
                    if (!mw->pins[pi].active) continue;

                    /* Distance from this vertex to the pin's RESTING position */
                    float prx, pry;
                    resting_pos(mw, px, py, img_w, img_h, &prx, &pry);
                    float dx = rx - prx;
                    float dy = ry - pry;
                    float d = sqrtf(dx * dx + dy * dy);
                    float w = gaussian_falloff(d, sigma);

                    /* Displacement the pin introduces */
                    float pin_dx = mw->pins[pi].pin_x - prx;
                    float pin_dy = mw->pins[pi].pin_y - pry;

                    disp_x += w * pin_dx;
                    disp_y += w * pin_dy;
                    total_w += w;
                }
            }

            if (total_w > 1e-6f) {
                /* Weighted average displacement, scaled by stiffness */
                float s = mw->stiffness;
                mw->deform_x[vi] = rx + (disp_x / total_w) * s;
                mw->deform_y[vi] = ry + (disp_y / total_w) * s;
            } else {
                mw->deform_x[vi] = rx;
                mw->deform_y[vi] = ry;
            }
        }
    }
}

/* Bilinear sample from a float RGBA frame at (sx, sy). Clamps to edges. */
static wb_px sample_bilinear(const wb_frame *f, float sx, float sy) {
    /* Clamp to valid sample range */
    if (sx < 0.0f) sx = 0.0f;
    if (sy < 0.0f) sy = 0.0f;
    if (sx > (float)(f->w - 1)) sx = (float)(f->w - 1);
    if (sy > (float)(f->h - 1)) sy = (float)(f->h - 1);

    int ix = (int)sx;
    int iy = (int)sy;
    float fx = sx - (float)ix;
    float fy = sy - (float)iy;

    /* Clamp indices for safe access */
    int ix1 = ix + 1;
    int iy1 = iy + 1;
    if (ix1 >= f->w) ix1 = f->w - 1;
    if (iy1 >= f->h) iy1 = f->h - 1;

    const wb_px *p00 = &f->px[iy * f->w + ix];
    const wb_px *p10 = &f->px[iy * f->w + ix1];
    const wb_px *p01 = &f->px[iy1 * f->w + ix];
    const wb_px *p11 = &f->px[iy1 * f->w + ix1];

    wb_px out;
    float w00 = (1.0f - fx) * (1.0f - fy);
    float w10 = fx * (1.0f - fy);
    float w01 = (1.0f - fx) * fy;
    float w11 = fx * fy;

    out.r = p00->r * w00 + p10->r * w10 + p01->r * w01 + p11->r * w11;
    out.g = p00->g * w00 + p10->g * w10 + p01->g * w01 + p11->g * w11;
    out.b = p00->b * w00 + p10->b * w10 + p01->b * w01 + p11->b * w11;
    out.a = p00->a * w00 + p10->a * w10 + p01->a * w01 + p11->a * w11;
    return out;
}

/* ---- pull ------------------------------------------------------------- */

static wb_frame *mesh_warp_pull(wb_node *self, double t,
                                 int rx, int ry, int rw, int rh, int phase) {
    (void)t; (void)rx; (void)ry; (void)rw; (void)rh; (void)phase;
    wb_mesh_warp_t *mw = (wb_mesh_warp_t *)self->user;
    if (!mw || self->n_inputs < 1 || !self->inputs[0]) return NULL;

    /* Pull input at the requested roi (pass through) */
    wb_frame *in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
    if (!in) return NULL;

    int w = in->w, h = in->h;
    if (w <= 0 || h <= 0) { wb_frame_free(in); return NULL; }

    /* Recompute deformed mesh for this frame size */
    mesh_warp_recompute(mw, w, h);

    /* Allocate output frame */
    wb_frame *out = wb_frame_alloc(w, h);
    if (!out) { wb_frame_free(in); return NULL; }

    /* For each output pixel, find its cell in the deformed mesh,
     * bilinearly interpolate the source position, and sample the input. */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            /* Normalized position in the grid (0..grid_w, 0..grid_h) */
            float gx = (float)x / (float)(w - 1) * (float)(mw->grid_w);
            float gy = (float)y / (float)(h - 1) * (float)(mw->grid_h);

            /* Clamp to valid cell range */
            if (gx > (float)(mw->grid_w - 1)) gx = (float)(mw->grid_w - 1);
            if (gy > (float)(mw->grid_h - 1)) gy = (float)(mw->grid_h - 1);
            if (gx < 0.0f) gx = 0.0f;
            if (gy < 0.0f) gy = 0.0f;

            int cx = (int)gx;
            int cy = (int)gy;
            if (cx >= mw->grid_w - 1) cx = mw->grid_w - 2;
            if (cy >= mw->grid_h - 1) cy = mw->grid_h - 2;
            float fx = gx - (float)cx;
            float fy = gy - (float)cy;

            /* Bilinearly interpolate the deformed vertex positions
             * to get the source position in the input image */
            /* Vertex indices for this cell's corners */
            int v00 = cy * mw->vert_w + cx;
            int v10 = cy * mw->vert_w + (cx + 1);
            int v01 = (cy + 1) * mw->vert_w + cx;
            int v11 = (cy + 1) * mw->vert_w + (cx + 1);

            float src_x = mw->deform_x[v00] * (1.0f - fx) * (1.0f - fy)
                        + mw->deform_x[v10] * fx * (1.0f - fy)
                        + mw->deform_x[v01] * (1.0f - fx) * fy
                        + mw->deform_x[v11] * fx * fy;

            float src_y = mw->deform_y[v00] * (1.0f - fx) * (1.0f - fy)
                        + mw->deform_y[v10] * fx * (1.0f - fy)
                        + mw->deform_y[v01] * (1.0f - fx) * fy
                        + mw->deform_y[v11] * fx * fy;

            /* Sample input at the source position */
            out->px[y * w + x] = sample_bilinear(in, src_x, src_y);
        }
    }

    wb_frame_free(in);
    return out;
}

static void mesh_warp_free(wb_node *self) {
    wb_mesh_warp_t *mw = (wb_mesh_warp_t *)self->user;
    if (!mw) return;
    free(mw->deform_x);
    free(mw->deform_y);
    free(mw->pins);
    free(mw);
    self->user = NULL;
}

/* ---- public API ------------------------------------------------------- */

wb_node *wb_node_effect_mesh_warp(int grid_w, int grid_h) {
    /* Clamp grid dimensions */
    if (grid_w < 2) grid_w = 2;
    if (grid_h < 2) grid_h = 2;
    if (grid_w > WB_MESH_WARP_MAX_GRID) grid_w = WB_MESH_WARP_MAX_GRID;
    if (grid_h > WB_MESH_WARP_MAX_GRID) grid_h = WB_MESH_WARP_MAX_GRID;

    wb_node *n = wb_node_create(WB_NODE_EFFECT, "mesh_warp");
    if (!n) return NULL;

    wb_mesh_warp_t *mw = calloc(1, sizeof(*mw));
    if (!mw) { wb_node_destroy(n); return NULL; }

    mw->grid_w = grid_w;
    mw->grid_h = grid_h;
    mw->vert_w = grid_w + 1;
    mw->vert_h = grid_h + 1;
    mw->stiffness = 0.5f;  /* default medium stiffness */

    size_t nverts = (size_t)mw->vert_w * (size_t)mw->vert_h;
    mw->deform_x = calloc(nverts, sizeof(float));
    mw->deform_y = calloc(nverts, sizeof(float));
    mw->pins = calloc(nverts, sizeof(wb_mesh_pin_t));

    if (!mw->deform_x || !mw->deform_y || !mw->pins) {
        free(mw->deform_x); free(mw->deform_y); free(mw->pins); free(mw);
        wb_node_destroy(n);
        return NULL;
    }

    n->user = mw;
    n->pull = mesh_warp_pull;
    n->free = mesh_warp_free;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node *));
    if (!n->inputs) {
        mesh_warp_free(n);
        wb_node_destroy(n);
        return NULL;
    }

    return n;
}

void wb_node_effect_mesh_warp_set_pin(wb_node *n, int grid_x, int grid_y,
                                       float pin_x, float pin_y) {
    if (!n || n->kind != WB_NODE_EFFECT) return;
    wb_mesh_warp_t *mw = (wb_mesh_warp_t *)n->user;
    if (!mw) return;
    if (grid_x < 0 || grid_x >= mw->vert_w) return;
    if (grid_y < 0 || grid_y >= mw->vert_h) return;

    int idx = grid_y * mw->vert_w + grid_x;
    mw->pins[idx].active = 1;
    mw->pins[idx].pin_x = pin_x;
    mw->pins[idx].pin_y = pin_y;
}

void wb_node_effect_mesh_warp_set_stiffness(wb_node *n, float stiffness) {
    if (!n || n->kind != WB_NODE_EFFECT) return;
    wb_mesh_warp_t *mw = (wb_mesh_warp_t *)n->user;
    if (!mw) return;
    if (stiffness < 0.0f) stiffness = 0.0f;
    if (stiffness > 1.0f) stiffness = 1.0f;
    mw->stiffness = stiffness;
}

void wb_node_effect_mesh_warp_clear_pins(wb_node *n) {
    if (!n || n->kind != WB_NODE_EFFECT) return;
    wb_mesh_warp_t *mw = (wb_mesh_warp_t *)n->user;
    if (!mw) return;
    memset(mw->pins, 0, (size_t)mw->vert_w * (size_t)mw->vert_h * sizeof(wb_mesh_pin_t));
}