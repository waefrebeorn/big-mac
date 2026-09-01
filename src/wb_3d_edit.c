/* wb_3d_edit.c — stereoscopic 3D editing for Vegas 10-style 3D workflows.
 *
 * Supports loading left+right eye sources, depth adjustment effects, and
 * output formatting into the four standard stereo modes:
 *   - Anaglyph (red/cyan)
 *   - Side-by-side (left|right)
 *   - Top-bottom (left over right)
 *   - Checkerboard (interleaved tiles for DLP displays)
 *
 * Pure C11, operates on RGBA uint8 buffers. Designed for the Big Mac
 * compositor pipeline: source nodes pull decoded frames, the depth effect
 * node adjusts parallax, and the composite node formats the final output.
 */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

/* ================================================================ */
/* 3D output mode enum                                               */
/* ================================================================ */

typedef enum {
    WB_3D_ANAGLYPH = 0,      /* red/cyan glasses */
    WB_3D_SIDE_BY_SIDE,      /* left|right, half width each */
    WB_3D_TOP_BOTTOM,        /* left over right, half height each */
    WB_3D_CHECKERBOARD       /* interleaved 8x8 tiles for DLP */
} wb_3d_mode_t;

/* ================================================================ */
/* Stereo source state (left + right eye decoders)                   */
/* ================================================================ */

typedef struct {
    char   left_path[512];
    char   right_path[512];
    int    width;          /* per-eye width */
    int    height;         /* per-eye height */
    int    active;         /* 1 once both sources validated */
} wb_source_stereo3d_t;

/* Create a stereo 3D source. Paths may be identical (mono→duplicate)
 * or distinct (true stereo pair). For offline/test use we don't require
 * real decoders — the source records dimensions and validates. */
wb_source_stereo3d_t *wb_node_source_stereo3d_create(const char *left_path,
                                                      const char *right_path,
                                                      int width, int height) {
    if (!left_path || !left_path[0]) return NULL;
    if (width <= 0 || height <= 0) return NULL;

    wb_source_stereo3d_t *s = (wb_source_stereo3d_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;

    snprintf(s->left_path, sizeof(s->left_path), "%s", left_path);
    snprintf(s->right_path, sizeof(s->right_path), "%s",
             right_path && right_path[0] ? right_path : left_path);
    s->width  = width;
    s->height = height;
    s->active = 1;
    return s;
}

void wb_node_source_stereo3d_destroy(wb_source_stereo3d_t *s) {
    free(s);
}

int wb_node_source_stereo3d_width(const wb_source_stereo3d_t *s) {
    return s ? s->width : 0;
}

int wb_node_source_stereo3d_height(const wb_source_stereo3d_t *s) {
    return s ? s->height : 0;
}

int wb_node_source_stereo3d_active(const wb_source_stereo3d_t *s) {
    return s ? s->active : 0;
}

/* Pull a stereo pair: fill left_buf and right_buf with RGBA data.
 * For real use this would call wb_video_decoder per eye. Here we
 * synthesize a simple test pattern so the pipeline can be validated
 * headlessly. Returns 0 on success. */
int wb_node_source_stereo3d_pull(wb_source_stereo3d_t *s,
                                  uint8_t *left_buf,
                                  uint8_t *right_buf,
                                  int width, int height) {
    if (!s || !left_buf || !right_buf) return -1;
    if (width != s->width || height != s->height) return -1;

    int n = width * height;
    for (int i = 0; i < n; i++) {
        int p = i * 4;
        /* Left eye: gradient with slight warm tint */
        left_buf[p+0] = (uint8_t)((i * 7) & 0xFF);  /* R */
        left_buf[p+1] = (uint8_t)((i * 3) & 0xFF);  /* G */
        left_buf[p+2] = (uint8_t)((i * 1) & 0xFF);  /* B */
        left_buf[p+3] = 255;                         /* A */

        /* Right eye: same gradient shifted horizontally (parallax) */
        int shift = 4;  /* 4px parallax for test */
        int src = (i + shift) % n;
        right_buf[p+0] = (uint8_t)((src * 7) & 0xFF);
        right_buf[p+1] = (uint8_t)((src * 3) & 0xFF);
        right_buf[p+2] = (uint8_t)((src * 1) & 0xFF);
        right_buf[p+3] = 255;
    }
    return 0;
}

/* ================================================================ */
/* Depth adjustment effect                                           */
/* ================================================================ */

typedef struct {
    float depth;           /* -1.0 (near) .. 0.0 (neutral) .. 1.0 (far) */
    float convergence;     /* screen-plane offset in pixels */
    int   width, height;
} wb_effect_3d_depth_t;

wb_effect_3d_depth_t *wb_node_effect_3d_depth_create(int width, int height) {
    if (width <= 0 || height <= 0) return NULL;
    wb_effect_3d_depth_t *d = (wb_effect_3d_depth_t *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->depth       = 0.0f;
    d->convergence = 0.0f;
    d->width       = width;
    d->height      = height;
    return d;
}

void wb_node_effect_3d_depth_destroy(wb_effect_3d_depth_t *d) {
    free(d);
}

void wb_node_effect_3d_depth_set(wb_effect_3d_depth_t *d, float depth) {
    if (!d) return;
    d->depth = fmaxf(-1.0f, fminf(1.0f, depth));
}

void wb_node_effect_3d_depth_set_convergence(wb_effect_3d_depth_t *d, float px) {
    if (!d) return;
    d->convergence = px;
}

float wb_node_effect_3d_depth_get(const wb_effect_3d_depth_t *d) {
    return d ? d->depth : 0.0f;
}

/* Apply depth shift: horizontally offset the right eye relative to left
 * to increase/decrease parallax. Positive depth = more parallax (closer
 * objects pop out). The output buffers are the same size; shifted pixels
 * are clamped to the edge. */
void wb_node_effect_3d_depth_apply(const wb_effect_3d_depth_t *d,
                                    const uint8_t *left_eye,
                                    const uint8_t *right_eye,
                                    uint8_t *left_out,
                                    uint8_t *right_out,
                                    int width, int height) {
    if (!d || !left_eye || !right_eye || !left_out || !right_out) return;
    if (width != d->width || height != d->height) return;

    int n = width * height;

    /* Left eye: shift by -depth * scale + convergence */
    int left_shift = (int)(-d->depth * 8.0f + d->convergence);

    /* Right eye: shift by +depth * scale - convergence */
    int right_shift = (int)(d->depth * 8.0f - d->convergence);

    memset(left_out, 0, (size_t)n * 4);
    memset(right_out, 0, (size_t)n * 4);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int dst = (y * width + x) * 4;

            /* Left eye: sample from x - left_shift */
            int lx = x - left_shift;
            if (lx >= 0 && lx < width) {
                int lsrc = (y * width + lx) * 4;
                left_out[dst+0] = left_eye[lsrc+0];
                left_out[dst+1] = left_eye[lsrc+1];
                left_out[dst+2] = left_eye[lsrc+2];
                left_out[dst+3] = left_eye[lsrc+3];
            }

            /* Right eye: sample from x - right_shift */
            int rx = x - right_shift;
            if (rx >= 0 && rx < width) {
                int rsrc = (y * width + rx) * 4;
                right_out[dst+0] = right_eye[rsrc+0];
                right_out[dst+1] = right_eye[rsrc+1];
                right_out[dst+2] = right_eye[rsrc+2];
                right_out[dst+3] = right_eye[rsrc+3];
            }
        }
    }
}

/* ================================================================ */
/* 3D composite (output formatting)                                  */
/* ================================================================ */

typedef struct {
    wb_3d_mode_t mode;
    int          tile_size;    /* for checkerboard mode */
} wb_composite_3d_t;

wb_composite_3d_t *wb_node_composite_3d_create(wb_3d_mode_t mode) {
    wb_composite_3d_t *c = (wb_composite_3d_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->mode = mode;
    c->tile_size = 8;  /* default 8x8 checkerboard tiles */
    return c;
}

void wb_node_composite_3d_destroy(wb_composite_3d_t *c) {
    free(c);
}

void wb_node_composite_3d_set_mode(wb_composite_3d_t *c, wb_3d_mode_t mode) {
    if (c) c->mode = mode;
}

void wb_node_composite_3d_set_tile_size(wb_composite_3d_t *c, int size) {
    if (c && size > 0) c->tile_size = size;
}

wb_3d_mode_t wb_node_composite_3d_get_mode(const wb_composite_3d_t *c) {
    return c ? c->mode : WB_3D_ANAGLYPH;
}

/* Get the output dimensions for a given mode. Side-by-side doubles the
 * width; top-bottom doubles the height; anaglyph and checkerboard keep
 * the per-eye dimensions. */
void wb_node_composite_3d_output_size(const wb_composite_3d_t *c,
                                       int eye_w, int eye_h,
                                       int *out_w, int *out_h) {
    if (!c) { *out_w = eye_w; *out_h = eye_h; return; }
    switch (c->mode) {
    case WB_3D_SIDE_BY_SIDE:
        *out_w = eye_w * 2;
        *out_h = eye_h;
        break;
    case WB_3D_TOP_BOTTOM:
        *out_w = eye_w;
        *out_h = eye_h * 2;
        break;
    case WB_3D_ANAGLYPH:
    case WB_3D_CHECKERBOARD:
    default:
        *out_w = eye_w;
        *out_h = eye_h;
        break;
    }
}

/* Composite: combine left + right eye into the configured output format.
 * out_buf must be large enough for the output dimensions. Returns 0 on
 * success. */
int wb_node_composite_3d_apply(const wb_composite_3d_t *c,
                                const uint8_t *left_eye,
                                const uint8_t *right_eye,
                                uint8_t *out_buf,
                                int eye_w, int eye_h) {
    if (!c || !left_eye || !right_eye || !out_buf) return -1;
    if (eye_w <= 0 || eye_h <= 0) return -1;

    int out_w, out_h;
    wb_node_composite_3d_output_size(c, eye_w, eye_h, &out_w, &out_h);

    switch (c->mode) {
    case WB_3D_ANAGLYPH: {
        /* Red channel from left, green+blue from right */
        int n = eye_w * eye_h;
        for (int i = 0; i < n; i++) {
            int p = i * 4;
            out_buf[p+0] = left_eye[p+0];   /* R from left */
            out_buf[p+1] = right_eye[p+1];  /* G from right */
            out_buf[p+2] = right_eye[p+2];  /* B from right */
            out_buf[p+3] = 255;             /* fully opaque */
        }
        break;
    }

    case WB_3D_SIDE_BY_SIDE: {
        /* Left eye in left half, right eye in right half */
        for (int y = 0; y < eye_h; y++) {
            for (int x = 0; x < eye_w; x++) {
                int src = (y * eye_w + x) * 4;
                int dst = (y * out_w + x) * 4;
                out_buf[dst+0] = left_eye[src+0];
                out_buf[dst+1] = left_eye[src+1];
                out_buf[dst+2] = left_eye[src+2];
                out_buf[dst+3] = 255;

                int dst_r = (y * out_w + eye_w + x) * 4;
                out_buf[dst_r+0] = right_eye[src+0];
                out_buf[dst_r+1] = right_eye[src+1];
                out_buf[dst_r+2] = right_eye[src+2];
                out_buf[dst_r+3] = 255;
            }
        }
        break;
    }

    case WB_3D_TOP_BOTTOM: {
        /* Left eye in top half, right eye in bottom half */
        for (int y = 0; y < eye_h; y++) {
            for (int x = 0; x < eye_w; x++) {
                int src = (y * eye_w + x) * 4;

                int dst_top = (y * out_w + x) * 4;
                out_buf[dst_top+0] = left_eye[src+0];
                out_buf[dst_top+1] = left_eye[src+1];
                out_buf[dst_top+2] = left_eye[src+2];
                out_buf[dst_top+3] = 255;

                int dst_bot = ((eye_h + y) * out_w + x) * 4;
                out_buf[dst_bot+0] = right_eye[src+0];
                out_buf[dst_bot+1] = right_eye[src+1];
                out_buf[dst_bot+2] = right_eye[src+2];
                out_buf[dst_bot+3] = 255;
            }
        }
        break;
    }

    case WB_3D_CHECKERBOARD: {
        /* Interleave 8x8 tiles: even tiles = left, odd tiles = right */
        int ts = c->tile_size;
        int n = eye_w * eye_h;
        memset(out_buf, 0, (size_t)n * 4);

        for (int y = 0; y < eye_h; y++) {
            for (int x = 0; x < eye_w; x++) {
                int tile_x = x / ts;
                int tile_y = y / ts;
                int is_left_tile = ((tile_x + tile_y) & 1) == 0;

                int p = (y * eye_w + x) * 4;
                const uint8_t *src = is_left_tile ? left_eye : right_eye;
                out_buf[p+0] = src[p+0];
                out_buf[p+1] = src[p+1];
                out_buf[p+2] = src[p+2];
                out_buf[p+3] = 255;
            }
        }
        break;
    }
    }

    return 0;
}