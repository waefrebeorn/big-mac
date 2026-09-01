/* wb_shape_node.c — Shape layer source nodes (After Effects parity)
 * R088: vector primitive generators — rect, ellipse, polygon, star, path
 *
 * Each node is a WB_NODE_SOURCE that renders a vector shape to a wb_frame
 * with anti-aliased edges. Supports fill color, stroke color, stroke width.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_compositor.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    wb_node base;
    int w, h;
    float fill_r, fill_g, fill_b, fill_a;
    float stroke_r, stroke_g, stroke_b, stroke_a;
    float stroke_width;
    int shape_type; /* 0=rect, 1=ellipse, 2=polygon, 3=star, 4=path */
    int sides;       /* for polygon */
    int points;      /* for star */
    float inner_radius; /* for star */
    float outer_radius; /* for star */
    char path_cmd[1024]; /* for SVG path */
} wb_shape_node;

/* Helper: set fill color */
static void set_fill(wb_shape_node *n, float r, float g, float b, float a) {
    n->fill_r = r; n->fill_g = g; n->fill_b = b; n->fill_a = a;
}

/* Helper: set stroke color */
static void set_stroke(wb_shape_node *n, float r, float g, float b, float a, float w) {
    n->stroke_r = r; n->stroke_g = g; n->stroke_b = b; n->stroke_a = a;
    n->stroke_width = w;
}

/* Point-in-polygon test */
static int point_in_polygon(float px, float py, const float *vx, const float *vy, int n) {
    int inside = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if (((vy[i] > py) != (vy[j] > py)) &&
            (px < (vx[j] - vx[i]) * (py - vy[i]) / (vy[j] - vy[i]) + vx[i]))
            inside = !inside;
    }
    return inside;
}

/* Distance from point to line segment */
static float dist_to_segment(float px, float py, float x1, float y1, float x2, float y2) {
    float dx = x2 - x1, dy = y2 - y1;
    float len2 = dx * dx + dy * dy;
    if (len2 < 1e-10f) {
        float ex = px - x1, ey = py - y1;
        return sqrtf(ex * ex + ey * ey);
    }
    float t = ((px - x1) * dx + (py - y1) * dy) / len2;
    t = fminf(1.0f, fmaxf(0.0f, t));
    float cx = x1 + t * dx, cy = y1 + t * dy;
    float ex = px - cx, ey = py - cy;
    return sqrtf(ex * ex + ey * ey);
}

/* Signed distance to rounded rectangle */
static float sd_rounded_rect(float px, float py, float cx, float cy, float hw, float hh, float r) {
    float qx = fabsf(px - cx) - hw + r;
    float qy = fabsf(py - cy) - hh + r;
    float outside = sqrtf(fmaxf(qx, 0) * fmaxf(qx, 0) + fmaxf(qy, 0) * fmaxf(qy, 0));
    float inside = fminf(fmaxf(qx, qy), 0.0f);
    return outside + inside - r;
}

/* Signed distance to ellipse */
static float sd_ellipse(float px, float py, float cx, float cy, float rx, float ry) {
    float dx = (px - cx) / rx;
    float dy = (py - cy) / ry;
    return (sqrtf(dx * dx + dy * dy) - 1.0f) * fminf(rx, ry);
}

/* Render shape to frame */
static void render_shape(wb_shape_node *n, wb_frame *f) {
    int w = f->roi_w;
    int h = f->roi_h;
    if (w <= 0 || h <= 0) return;

    float cx = w * 0.5f;
    float cy = h * 0.5f;

    /* Generate polygon vertices for polygon/star shapes */
    float vx[64], vy[64];
    int vcount = 0;

    if (n->shape_type == 2) { /* polygon */
        vcount = n->sides;
        if (vcount > 64) vcount = 64;
        if (vcount < 3) vcount = 3;
        float radius = fminf(cx, cy) * 0.9f;
        for (int i = 0; i < vcount; i++) {
            float angle = -M_PI * 0.5f + 2.0f * M_PI * i / vcount;
            vx[i] = cx + radius * cosf(angle);
            vy[i] = cy + radius * sinf(angle);
        }
    } else if (n->shape_type == 3) { /* star */
        vcount = n->points * 2;
        if (vcount > 64) vcount = 64;
        float ir = fminf(cx, cy) * 0.9f * n->inner_radius;
        float or2 = fminf(cx, cy) * 0.9f * n->outer_radius;
        for (int i = 0; i < vcount; i++) {
            float angle = -M_PI * 0.5f + M_PI * i / n->points;
            float r = (i % 2 == 0) ? or2 : ir;
            vx[i] = cx + r * cosf(angle);
            vy[i] = cy + r * sinf(angle);
        }
    }

    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int idx = py * w + px;
            float fr = 0, fg = 0, fb = 0, fa = 0;

            if (n->shape_type == 0) { /* rect */
                float d = sd_rounded_rect(px + 0.5f, py + 0.5f, cx, cy, cx * 0.45f, cy * 0.45f, 0);
                fa = 1.0f - fmaxf(0, fminf(1, d + 0.5f));
            } else if (n->shape_type == 1) { /* ellipse */
                float d = sd_ellipse(px + 0.5f, py + 0.5f, cx, cy, cx * 0.45f, cy * 0.45f);
                fa = 1.0f - fmaxf(0, fminf(1, d + 0.5f));
            } else if (n->shape_type == 2 || n->shape_type == 3) { /* polygon/star */
                int inside = point_in_polygon(px + 0.5f, py + 0.5f, vx, vy, vcount);
                fa = inside ? 1.0f : 0;

                /* Anti-alias: check distance to edges */
                if (!inside) {
                    float min_dist = 999;
                    for (int i = 0; i < vcount; i++) {
                        int j = (i + 1) % vcount;
                        float d = dist_to_segment(px + 0.5f, py + 0.5f, vx[i], vy[i], vx[j], vy[j]);
                        if (d < min_dist) min_dist = d;
                    }
                    if (min_dist < 1.0f) fa = 1.0f - min_dist;
                }
            } else { /* path — default to rect for now */
                float d = sd_rounded_rect(px + 0.5f, py + 0.5f, cx, cy, cx * 0.45f, cy * 0.45f, 0);
                fa = 1.0f - fmaxf(0, fminf(1, d + 0.5f));
            }

            fr = n->fill_r; fg = n->fill_g; fb = n->fill_b;
            fa *= n->fill_a;

            /* Stroke */
            if (n->stroke_width > 0 && fa < 1.0f) {
                float stroke_edge = 1.0f - n->stroke_width;
                if (fa > stroke_edge) {
                    fr = n->stroke_r; fg = n->stroke_g; fb = n->stroke_b;
                    fa = n->stroke_a;
                }
            }

            f->px[idx].r = fr;
            f->px[idx].g = fg;
            f->px[idx].b = fb;
            f->px[idx].a = fa;
        }
    }
}

static wb_frame *shape_pull(wb_node *node, double t, int rx, int ry, int rw, int rh) {
    wb_shape_node *n = (wb_shape_node *)node;
    int w = n->w > 0 ? n->w : rw;
    int h = n->h > 0 ? n->h : rh;
    wb_frame *f = wb_frame_alloc(w, h);
    if (!f) return NULL;
    f->roi_x = rx; f->roi_y = ry; f->roi_w = w; f->roi_h = h;
    render_shape(n, f);
    return f;
}

static void shape_free(wb_node *node) {
    if (node) free(node);
}

/* Factory functions */

wb_node *wb_node_source_shape_rect(int w, int h) {
    wb_shape_node *n = (wb_shape_node *)calloc(1, sizeof(wb_shape_node));
    if (!n) return NULL;
    n->base.kind = WB_NODE_SOURCE;
    n->base.n_inputs = 0;
    n->base.pull = shape_pull;
    n->base.free = shape_free;
    n->w = w; n->h = h;
    n->shape_type = 0;
    set_fill(n, 1, 1, 1, 1);
    set_stroke(n, 0, 0, 0, 1, 0);
    snprintf(n->base.id, sizeof(n->base.id), "shape_rect_%dx%d", w, h);
    return (wb_node *)n;
}

wb_node *wb_node_source_shape_ellipse(int w, int h) {
    wb_shape_node *n = (wb_shape_node *)calloc(1, sizeof(wb_shape_node));
    if (!n) return NULL;
    n->base.kind = WB_NODE_SOURCE;
    n->base.n_inputs = 0;
    n->base.pull = shape_pull;
    n->base.free = shape_free;
    n->w = w; n->h = h;
    n->shape_type = 1;
    set_fill(n, 1, 1, 1, 1);
    set_stroke(n, 0, 0, 0, 1, 0);
    snprintf(n->base.id, sizeof(n->base.id), "shape_ellipse_%dx%d", w, h);
    return (wb_node *)n;
}

wb_node *wb_node_source_shape_polygon(int w, int h, int sides) {
    wb_shape_node *n = (wb_shape_node *)calloc(1, sizeof(wb_shape_node));
    if (!n) return NULL;
    n->base.kind = WB_NODE_SOURCE;
    n->base.n_inputs = 0;
    n->base.pull = shape_pull;
    n->base.free = shape_free;
    n->w = w; n->h = h;
    n->shape_type = 2;
    n->sides = sides > 2 ? sides : 6;
    set_fill(n, 1, 1, 1, 1);
    set_stroke(n, 0, 0, 0, 1, 0);
    snprintf(n->base.id, sizeof(n->base.id), "shape_polygon_%d", sides);
    return (wb_node *)n;
}

wb_node *wb_node_source_shape_star(int w, int h, int points, float inner, float outer) {
    wb_shape_node *n = (wb_shape_node *)calloc(1, sizeof(wb_shape_node));
    if (!n) return NULL;
    n->base.kind = WB_NODE_SOURCE;
    n->base.n_inputs = 0;
    n->base.pull = shape_pull;
    n->base.free = shape_free;
    n->w = w; n->h = h;
    n->shape_type = 3;
    n->points = points > 2 ? points : 5;
    n->inner_radius = inner > 0 ? inner : 0.4f;
    n->outer_radius = outer > 0 ? outer : 1.0f;
    set_fill(n, 1, 1, 1, 1);
    set_stroke(n, 0, 0, 0, 1, 0);
    snprintf(n->base.id, sizeof(n->base.id), "shape_star_%d", points);
    return (wb_node *)n;
}

/* Set shape colors */
void wb_node_shape_set_fill(wb_node *node, float r, float g, float b, float a) {
    if (!node) return;
    wb_shape_node *n = (wb_shape_node *)node;
    set_fill(n, r, g, b, a);
}

void wb_node_shape_set_stroke(wb_node *node, float r, float g, float b, float a, float width) {
    if (!node) return;
    wb_shape_node *n = (wb_shape_node *)node;
    set_stroke(n, r, g, b, a, width);
}
