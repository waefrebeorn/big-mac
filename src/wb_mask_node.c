/* wb_mask_node.c — Per-layer mask effect node (R090)
 *
 * Mask node: takes a source image (input 0) and an optional mask source
 * (input 1). If no mask source is connected, uses an internal path-based
 * mask rendered via scanline rasterization. Supports feather (box blur
 * on alpha), expand (dilation/erosion on alpha), and invert.
 *
 * The final mask alpha is multiplied into the source alpha channel.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_compositor.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MASK_MAX_PATH_VERTS 256

typedef struct {
    float x, y;
} mask_vertex_t;

typedef struct {
    wb_node base;
    int w, h;
    float feather_px;
    float expand_px;
    int invert;
    /* Path-based mask (SVG-style M/L/Z commands) */
    mask_vertex_t path_verts[MASK_MAX_PATH_VERTS];
    int path_count;
    int path_closed;
    /* Pre-rendered mask alpha (path-only, before feather/expand) */
    float *mask_alpha;
    int mask_dirty;
} wb_mask_node;

/* ---- SVG path parsing ----------------------------------------------- */

static void parse_path(wb_mask_node *n, const char *path_str) {
    n->path_count = 0;
    n->path_closed = 0;
    n->mask_dirty = 1;

    if (!path_str || !*path_str) return;

    const char *p = path_str;
    float cur_x = 0, cur_y = 0;
    float start_x = 0, start_y = 0;
    char cmd = 0;

    while (*p) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;

        /* Check for command letter */
        if (*p == 'M' || *p == 'm' || *p == 'L' || *p == 'l' ||
            *p == 'H' || *p == 'h' || *p == 'V' || *p == 'v' ||
            *p == 'Z' || *p == 'z' || *p == 'C' || *p == 'c' ||
            *p == 'Q' || *p == 'q') {
            cmd = *p;
            p++;
        }
        /* If no command, repeat previous (implicit lineto for L/l) */

        float vals[6];
        int nvals = 0;

        /* Parse numbers */
        while (*p && nvals < 6) {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (!*p) break;
            char *end;
            vals[nvals] = strtof(p, &end);
            if (end == p) break;
            p = end;
            nvals++;
        }

        switch (cmd) {
        case 'M': /* absolute moveto */
            if (nvals >= 2) {
                cur_x = vals[0]; cur_y = vals[1];
                start_x = cur_x; start_y = cur_y;
                /* Add the moveto point as first vertex */
                if (n->path_count < MASK_MAX_PATH_VERTS) {
                    n->path_verts[n->path_count].x = cur_x;
                    n->path_verts[n->path_count].y = cur_y;
                    n->path_count++;
                }
                /* Subsequent coords become implicit lineto */
                cmd = 'L';
            }
            break;
        case 'm': /* relative moveto */
            if (nvals >= 2) {
                cur_x += vals[0]; cur_y += vals[1];
                start_x = cur_x; start_y = cur_y;
                /* Add the moveto point as first vertex */
                if (n->path_count < MASK_MAX_PATH_VERTS) {
                    n->path_verts[n->path_count].x = cur_x;
                    n->path_verts[n->path_count].y = cur_y;
                    n->path_count++;
                }
                cmd = 'l';
            }
            break;
        case 'L': /* absolute lineto */
            if (nvals >= 2 && n->path_count < MASK_MAX_PATH_VERTS) {
                cur_x = vals[0]; cur_y = vals[1];
                n->path_verts[n->path_count].x = cur_x;
                n->path_verts[n->path_count].y = cur_y;
                n->path_count++;
            }
            break;
        case 'l': /* relative lineto */
            if (nvals >= 2 && n->path_count < MASK_MAX_PATH_VERTS) {
                cur_x += vals[0]; cur_y += vals[1];
                n->path_verts[n->path_count].x = cur_x;
                n->path_verts[n->path_count].y = cur_y;
                n->path_count++;
            }
            break;
        case 'H': /* horizontal lineto */
            if (nvals >= 1 && n->path_count < MASK_MAX_PATH_VERTS) {
                cur_x = vals[0];
                n->path_verts[n->path_count].x = cur_x;
                n->path_verts[n->path_count].y = cur_y;
                n->path_count++;
            }
            break;
        case 'h': /* relative horizontal lineto */
            if (nvals >= 1 && n->path_count < MASK_MAX_PATH_VERTS) {
                cur_x += vals[0];
                n->path_verts[n->path_count].x = cur_x;
                n->path_verts[n->path_count].y = cur_y;
                n->path_count++;
            }
            break;
        case 'V': /* vertical lineto */
            if (nvals >= 1 && n->path_count < MASK_MAX_PATH_VERTS) {
                cur_y = vals[0];
                n->path_verts[n->path_count].x = cur_x;
                n->path_verts[n->path_count].y = cur_y;
                n->path_count++;
            }
            break;
        case 'v': /* relative vertical lineto */
            if (nvals >= 1 && n->path_count < MASK_MAX_PATH_VERTS) {
                cur_y += vals[0];
                n->path_verts[n->path_count].x = cur_x;
                n->path_verts[n->path_count].y = cur_y;
                n->path_count++;
            }
            break;
        case 'Z':
        case 'z':
            n->path_closed = 1;
            break;
        default:
            /* Skip unsupported commands (C, Q, etc.) */
            break;
        }
    }
}

/* ---- Scanline polygon rasterization --------------------------------- */

static void render_path_to_alpha(wb_mask_node *n, int w, int h) {
    if (!n->mask_alpha) {
        n->mask_alpha = (float *)calloc((size_t)w * h, sizeof(float));
    }
    if (!n->mask_alpha) return;

    memset(n->mask_alpha, 0, (size_t)w * h * sizeof(float));

    if (n->path_count < 3 || !n->path_closed) return;

    /* Scanline fill: for each row, find edge crossings */
    for (int py = 0; py < h; py++) {
        float y = py + 0.5f;
        float crossings[MASK_MAX_PATH_VERTS];
        int n_cross = 0;

        for (int i = 0; i < n->path_count; i++) {
            int j = (i + 1) % n->path_count;
            float y0 = n->path_verts[i].y;
            float y1 = n->path_verts[j].y;

            /* Check if this edge crosses the scanline */
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                /* Compute x intersection */
                float t = (y - y0) / (y1 - y0);
                float x = n->path_verts[i].x + t * (n->path_verts[j].x - n->path_verts[i].x);
                if (n_cross < MASK_MAX_PATH_VERTS) {
                    crossings[n_cross++] = x;
                }
            }
        }

        /* Sort crossings */
        for (int i = 0; i < n_cross - 1; i++) {
            for (int j = i + 1; j < n_cross; j++) {
                if (crossings[j] < crossings[i]) {
                    float tmp = crossings[i];
                    crossings[i] = crossings[j];
                    crossings[j] = tmp;
                }
            }
        }

        /* Fill between pairs */
        for (int i = 0; i + 1 < n_cross; i += 2) {
            int x_start = (int)floorf(crossings[i]);
            int x_end = (int)ceilf(crossings[i + 1]);
            if (x_start < 0) x_start = 0;
            if (x_end > w) x_end = w;
            for (int px = x_start; px < x_end; px++) {
                n->mask_alpha[py * w + px] = 1.0f;
            }
        }
    }

    n->mask_dirty = 0;
}

/* ---- Box blur on alpha channel -------------------------------------- */

static void apply_box_blur(float *alpha, int w, int h, float radius) {
    if (radius < 0.5f) return;

    int r = (int)ceilf(radius);
    if (r < 1) return;

    float *tmp = (float *)calloc((size_t)w * h, sizeof(float));
    if (!tmp) return;

    /* Horizontal pass */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float sum = 0;
            int count = 0;
            for (int dx = -r; dx <= r; dx++) {
                int sx = x + dx;
                if (sx >= 0 && sx < w) {
                    sum += alpha[y * w + sx];
                    count++;
                }
            }
            tmp[y * w + x] = sum / count;
        }
    }

    /* Vertical pass */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float sum = 0;
            int count = 0;
            for (int dy = -r; dy <= r; dy++) {
                int sy = y + dy;
                if (sy >= 0 && sy < h) {
                    sum += tmp[sy * w + x];
                    count++;
                }
            }
            alpha[y * w + x] = sum / count;
        }
    }

    free(tmp);
}

/* ---- Dilation / Erosion on alpha channel ---------------------------- */

static void apply_expand(float *alpha, int w, int h, float expand_px) {
    if (fabsf(expand_px) < 0.5f) return;

    int r = (int)ceilf(fabsf(expand_px));
    if (r < 1) return;

    float *tmp = (float *)calloc((size_t)w * h, sizeof(float));
    if (!tmp) return;

    if (expand_px > 0) {
        /* Dilation: max in neighborhood */
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float val = 0;
                for (int dy = -r; dy <= r; dy++) {
                    for (int dx = -r; dx <= r; dx++) {
                        int sx = x + dx, sy = y + dy;
                        if (sx >= 0 && sx < w && sy >= 0 && sy < h) {
                            if (alpha[sy * w + sx] > val)
                                val = alpha[sy * w + sx];
                        }
                    }
                }
                tmp[y * w + x] = val;
            }
        }
    } else {
        /* Erosion: min in neighborhood */
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float val = 1.0f;
                for (int dy = -r; dy <= r; dy++) {
                    for (int dx = -r; dx <= r; dx++) {
                        int sx = x + dx, sy = y + dy;
                        if (sx >= 0 && sx < w && sy >= 0 && sy < h) {
                            if (alpha[sy * w + sx] < val)
                                val = alpha[sy * w + sx];
                        }
                    }
                }
                tmp[y * w + x] = val;
            }
        }
    }

    memcpy(alpha, tmp, (size_t)w * h * sizeof(float));
    free(tmp);
}

/* ---- Pull function -------------------------------------------------- */

static wb_frame *mask_pull(wb_node *node, double t,
                           int rx, int ry, int rw, int rh, int phase) {
    (void)phase; /* we compute immediately; two-phase not needed for masks */
    wb_mask_node *n = (wb_mask_node *)node;

    /* Pull source (input 0) */
    wb_frame *src = NULL;
    if (node->inputs[0]) {
        src = node->inputs[0]->pull(node->inputs[0], t, rx, ry, rw, rh, phase);
    }
    if (!src) return NULL;

    /* Pull optional mask source (input 1) */
    wb_frame *mask_src = NULL;
    if (node->n_inputs > 1 && node->inputs[1]) {
        mask_src = node->inputs[1]->pull(node->inputs[1], t, rx, ry, rw, rh, phase);
    }

    /* Determine mask alpha */
    float *mask_alpha = NULL;
    float *local_alpha = NULL;

    if (mask_src) {
        /* Use mask source's alpha channel */
        local_alpha = (float *)malloc((size_t)rw * rh * sizeof(float));
        if (!local_alpha) {
            wb_frame_free(mask_src);
            return src;
        }
        for (int i = 0; i < rw * rh; i++) {
            local_alpha[i] = mask_src->px[i].a;
        }
        mask_alpha = local_alpha;
        wb_frame_free(mask_src);
    } else {
        /* Use path-based mask */
        if (n->mask_dirty) {
            render_path_to_alpha(n, n->w, n->h);
        }
        /* Extract ROI from pre-rendered mask */
        if (n->mask_alpha) {
            local_alpha = (float *)malloc((size_t)rw * rh * sizeof(float));
            if (!local_alpha) return src;
            for (int y = 0; y < rh; y++) {
                for (int x = 0; x < rw; x++) {
                    int sx = rx + x;
                    int sy = ry + y;
                    if (sx >= 0 && sx < n->w && sy >= 0 && sy < n->h) {
                        local_alpha[y * rw + x] = n->mask_alpha[sy * n->w + sx];
                    } else {
                        local_alpha[y * rw + x] = 0.0f;
                    }
                }
            }
            mask_alpha = local_alpha;
        }
    }

    /* Apply feather */
    if (mask_alpha && n->feather_px > 0.5f) {
        apply_box_blur(mask_alpha, rw, rh, n->feather_px);
    }

    /* Apply expand */
    if (mask_alpha && fabsf(n->expand_px) >= 0.5f) {
        apply_expand(mask_alpha, rw, rh, n->expand_px);
    }

    /* Apply invert */
    if (mask_alpha && n->invert) {
        for (int i = 0; i < rw * rh; i++) {
            mask_alpha[i] = 1.0f - mask_alpha[i];
        }
    }

    /* Multiply source alpha by mask alpha */
    if (mask_alpha) {
        for (int i = 0; i < rw * rh; i++) {
            src->px[i].a *= mask_alpha[i];
        }
        free(local_alpha);
    }

    return src;
}

/* ---- Destructor ----------------------------------------------------- */

static void mask_free(wb_node *node) {
    if (!node) return;
    wb_mask_node *n = (wb_mask_node *)node;
    if (n->mask_alpha) {
        free(n->mask_alpha);
        n->mask_alpha = NULL;
    }
    /* Note: we do NOT free(node) here — wb_node_destroy handles that */
}

/* ---- Public API ----------------------------------------------------- */

wb_node *wb_node_effect_mask(int w, int h) {
    wb_mask_node *n = (wb_mask_node *)calloc(1, sizeof(wb_mask_node));
    if (!n) return NULL;
    n->base.kind = WB_NODE_EFFECT;
    n->base.n_inputs = 2; /* input 0 = source, input 1 = optional mask source */
    n->base.inputs = (wb_node **)calloc(2, sizeof(wb_node *));
    n->base.pull = mask_pull;
    n->base.free = mask_free;
    n->w = w > 0 ? w : 1920;
    n->h = h > 0 ? h : 1080;
    n->feather_px = 0;
    n->expand_px = 0;
    n->invert = 0;
    n->path_count = 0;
    n->path_closed = 0;
    n->mask_alpha = NULL;
    n->mask_dirty = 1;
    snprintf(n->base.id, sizeof(n->base.id), "mask_%dx%d", n->w, n->h);
    return (wb_node *)n;
}

void wb_node_effect_mask_set_feather(wb_node *node, float feather_px) {
    if (!node) return;
    wb_mask_node *n = (wb_mask_node *)node;
    n->feather_px = feather_px > 0 ? feather_px : 0;
}

void wb_node_effect_mask_set_expand(wb_node *node, float expand_px) {
    if (!node) return;
    wb_mask_node *n = (wb_mask_node *)node;
    n->expand_px = expand_px;
}

void wb_node_effect_mask_set_invert(wb_node *node, int invert) {
    if (!node) return;
    wb_mask_node *n = (wb_mask_node *)node;
    n->invert = invert ? 1 : 0;
}

void wb_node_effect_mask_set_path(wb_node *node, const char *path_str) {
    if (!node) return;
    wb_mask_node *n = (wb_mask_node *)node;
    parse_path(n, path_str);
}