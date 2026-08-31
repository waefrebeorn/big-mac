/* wb_stabilize2.c — video stabilization (phase-correlation motion estimation).
 *
 * R077+: DaVinci Resolve-style motion estimation and smoothing.
 *
 * Algorithm:
 *   1. Convert RGBA frame to grayscale
 *   2. Phase correlation between consecutive frames (log-polar FFT omitted
 *      for zero-dep build; uses windowed block-matching via phase-correlation
 *      peak detection on a coarse grid)
 *   3. Track cumulative translation (dx, dy) per frame
 *   4. Smooth trajectory (exponential moving average on translation)
 *   5. Apply correction + crop to hide borders
 *
 * Pure C11, zero third-party deps. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

/* Phase-correlation grid: we divide the frame into PHASE_GRID x PHASE_GRID
 * blocks and estimate local motion per block, then take the median as the
 * global translation. This is robust to local motion (moving subjects). */
#define PHASE_GRID   8
#define BLOCK_SIZE   32   /* pixels per block for correlation search */
#define SEARCH_RAD   16   /* search radius in each direction */
#define GRAY_MAX     256

typedef struct {
    /* Grayscale reference frame */
    float   *prev_gray;         /* [width*height] */
    float   *curr_gray;         /* [width*height] */
    int      width, height;

    /* Cumulative translation tracking */
    float    cum_dx, cum_y;     /* cumulative raw translation */
    float    smooth_dx, smooth_y; /* smoothed (EMA) translation */

    /* Parameters */
    float    smoothing;         /* EMA alpha (0=none, 1=max smooth) */
    float    crop_percent;      /* fraction of border to crop */

    /* State */
    int      initialized;
    int      frame_count;
} wb_stabilize2_inst;

/* ---- grayscale conversion -------------------------------------------- */

static void rgba_to_gray(const uint8_t *rgba, float *gray, int w, int h) {
    int n = w * h;
    for (int i = 0; i < n; i++) {
        /* Rec. 601 luma: 0.299 R + 0.587 G + 0.114 B */
        gray[i] = 0.299f * rgba[i * 4]
                + 0.587f * rgba[i * 4 + 1]
                + 0.114f * rgba[i * 4 + 2];
    }
}

/* ---- phase-correlation motion estimation ----------------------------- */
/*
 * Simplified phase correlation: for each block in the grid, we compute the
 * cross-correlation peak between the reference and current frame patches.
 * The peak offset gives the local motion vector. We then take the median
 * of all block vectors as the global translation (robust to outliers).
 */

static float block_correlation(float *prev, float *curr,
                                int w, int h,
                                int bx, int by,
                                int *out_dx, int *out_dy) {
    /* Block center in reference */
    int cx = (bx * w) / PHASE_GRID + (w / PHASE_GRID) / 2;
    int cy = (by * h) / PHASE_GRID + (h / PHASE_GRID) / 2;

    int half = BLOCK_SIZE / 2;
    if (cx - half < 0) cx = half;
    if (cy - half < 0) cy = half;
    if (cx + half >= w) cx = w - half - 1;
    if (cy + half >= h) cy = h - half - 1;

    float best_corr = -1e30f;
    int best_dx = 0, best_dy = 0;

    for (int dy = -SEARCH_RAD; dy <= SEARCH_RAD; dy += 2) {
        for (int dx = -SEARCH_RAD; dx <= SEARCH_RAD; dx += 2) {
            float corr = 0;
            float ref_mean = 0, cur_mean = 0;
            int count = 0;

            /* Compute means */
            for (int y = 0; y < BLOCK_SIZE; y += 2) {
                for (int x = 0; x < BLOCK_SIZE; x += 2) {
                    int rx = cx - half + x;
                    int ry = cy - half + y;
                    ref_mean += prev[ry * w + rx];
                    int tx = rx + dx;
                    int ty = ry + dy;
                    if (tx >= 0 && tx < w && ty >= 0 && ty < h)
                        cur_mean += curr[ty * w + tx];
                    count++;
                }
            }
            ref_mean /= count;
            cur_mean /= count;

            /* Normalized cross-correlation */
            float num = 0, den1 = 0, den2 = 0;
            for (int y = 0; y < BLOCK_SIZE; y += 2) {
                for (int x = 0; x < BLOCK_SIZE; x += 2) {
                    int rx = cx - half + x;
                    int ry = cy - half + y;
                    float rv = prev[ry * w + rx] - ref_mean;
                    int tx = rx + dx;
                    int ty = ry + dy;
                    if (tx < 0 || tx >= w || ty < 0 || ty >= h) continue;
                    float cv = curr[ty * w + tx] - cur_mean;
                    num += rv * cv;
                    den1 += rv * rv;
                    den2 += cv * cv;
                }
            }

            float denom = sqrtf(den1 * den2);
            if (denom > 1e-6f)
                corr = num / denom;

            if (corr > best_corr) {
                best_corr = corr;
                best_dx = dx;
                best_dy = dy;
            }
        }
    }

    *out_dx = best_dx;
    *out_dy = best_dy;
    return best_corr;
}

/* Median of an int array (in-place partial sort) */
static int median_int(int *arr, int n) {
    /* Simple insertion sort for small n */
    for (int i = 1; i < n; i++) {
        int key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
    return arr[n / 2];
}

/* Estimate global translation via phase-correlation block matching */
static void estimate_motion(wb_stabilize2_inst *st,
                            float *prev, float *curr,
                            float *out_dx, float *out_dy) {
    int nblocks = PHASE_GRID * PHASE_GRID;
    int *dxs = (int *)malloc(nblocks * sizeof(int));
    int *dys = (int *)malloc(nblocks * sizeof(int));

    for (int by = 0; by < PHASE_GRID; by++) {
        for (int bx = 0; bx < PHASE_GRID; bx++) {
            int dx, dy;
            block_correlation(prev, curr, st->width, st->height,
                              bx, by, &dx, &dy);
            dxs[by * PHASE_GRID + bx] = dx;
            dys[by * PHASE_GRID + bx] = dy;
        }
    }

    *out_dx = (float)median_int(dxs, nblocks);
    *out_dy = (float)median_int(dys, nblocks);

    free(dxs);
    free(dys);
}

/* ---- apply stabilization transform ----------------------------------- */

static void apply_stabilize(uint8_t *frame_rgba, int w, int h,
                            float dx, float dy, float crop) {
    /* Compute crop region */
    int crop_x = (int)(w * crop * 0.5f);
    int crop_y = (int)(h * crop * 0.5f);
    int src_x0 = crop_x + (int)dx;
    int src_y0 = crop_y + (int)dy;

    /* Clamp source origin */
    if (src_x0 < 0) src_x0 = 0;
    if (src_y0 < 0) src_y0 = 0;
    if (src_x0 + w - 2 * crop_x > w) src_x0 = w - (w - 2 * crop_x);
    if (src_y0 + h - 2 * crop_y > h) src_y0 = h - (h - 2 * crop_y);

    /* Temporary buffer for the cropped+shifted result */
    int out_w = w - 2 * crop_x;
    int out_h = h - 2 * crop_y;
    uint8_t *tmp = (uint8_t *)calloc(w * h * 4, 1);

    /* Copy shifted region */
    for (int y = 0; y < out_h; y++) {
        for (int x = 0; x < out_w; x++) {
            int sx = src_x0 + x;
            int sy = src_y0 + y;
            if (sx >= 0 && sx < w && sy >= 0 && sy < h) {
                for (int c = 0; c < 4; c++) {
                    tmp[(y * out_w + x) * 4 + c] =
                        frame_rgba[(sy * w + sx) * 4 + c];
                }
            }
        }
    }

    /* Clear original frame to black (transparent) */
    memset(frame_rgba, 0, w * h * 4);

    /* Center the cropped result in the original frame */
    int dst_x0 = crop_x;
    int dst_y0 = crop_y;
    for (int y = 0; y < out_h; y++) {
        for (int x = 0; x < out_w; x++) {
            int dst_x = dst_x0 + x;
            int dst_y = dst_y0 + y;
            if (dst_x >= 0 && dst_x < w && dst_y >= 0 && dst_y < h) {
                for (int c = 0; c < 4; c++) {
                    frame_rgba[(dst_y * w + dst_x) * 4 + c] =
                        tmp[(y * out_w + x) * 4 + c];
                }
            }
        }
    }

    free(tmp);
}

/* ---- public API ------------------------------------------------------ */

void *wb_stabilize2_create(int width, int height) {
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
        return NULL;

    wb_stabilize2_inst *st = (wb_stabilize2_inst *)calloc(1, sizeof(*st));
    if (!st) return NULL;

    st->width = width;
    st->height = height;
    st->prev_gray = (float *)calloc(width * height, sizeof(float));
    st->curr_gray = (float *)calloc(width * height, sizeof(float));
    if (!st->prev_gray || !st->curr_gray) {
        free(st->prev_gray);
        free(st->curr_gray);
        free(st);
        return NULL;
    }

    st->smoothing = 0.85f;
    st->crop_percent = 0.05f;
    st->cum_dx = 0;
    st->cum_y = 0;
    st->smooth_dx = 0;
    st->smooth_y = 0;
    st->initialized = 0;
    st->frame_count = 0;

    return st;
}

void wb_stabilize2_destroy(void *s) {
    if (!s) return;
    wb_stabilize2_inst *st = (wb_stabilize2_inst *)s;
    free(st->prev_gray);
    free(st->curr_gray);
    free(st);
}

int wb_stabilize2_process(void *s, uint8_t *frame_rgba, int width, int height) {
    if (!s || !frame_rgba || width <= 0 || height <= 0)
        return -1;

    wb_stabilize2_inst *st = (wb_stabilize2_inst *)s;
    if (width != st->width || height != st->height)
        return -1;

    /* Convert current frame to grayscale */
    rgba_to_gray(frame_rgba, st->curr_gray, width, height);

    if (!st->initialized) {
        /* First frame: just store as reference */
        memcpy(st->prev_gray, st->curr_gray, width * height * sizeof(float));
        st->initialized = 1;
        st->frame_count = 1;
        return 0;
    }

    /* Estimate motion between prev and current */
    float dx, dy;
    estimate_motion(st, st->prev_gray, st->curr_gray, &dx, &dy);

    /* Accumulate translation */
    st->cum_dx += dx;
    st->cum_y += dy;

    /* Exponential moving average smoothing */
    float alpha = st->smoothing;
    st->smooth_dx = alpha * st->smooth_dx + (1.0f - alpha) * st->cum_dx;
    st->smooth_y = alpha * st->smooth_y + (1.0f - alpha) * st->cum_y;

    /* Correction = smoothed - cumulative (the offset to apply) */
    float corr_dx = st->smooth_dx - st->cum_dx;
    float corr_dy = st->smooth_y - st->cum_y;

    /* Apply stabilization transform */
    apply_stabilize(frame_rgba, width, height, corr_dx, corr_dy, st->crop_percent);

    /* Swap: current becomes previous */
    float *tmp = st->prev_gray;
    st->prev_gray = st->curr_gray;
    st->curr_gray = tmp;

    st->frame_count++;
    return 0;
}

void wb_stabilize2_set_smoothing(void *s, float smoothing) {
    if (!s) return;
    wb_stabilize2_inst *st = (wb_stabilize2_inst *)s;
    if (smoothing < 0.0f) smoothing = 0.0f;
    if (smoothing > 0.99f) smoothing = 0.99f;
    st->smoothing = smoothing;
}

void wb_stabilize2_set_crop(void *s, float crop_percent) {
    if (!s) return;
    wb_stabilize2_inst *st = (wb_stabilize2_inst *)s;
    if (crop_percent < 0.0f) crop_percent = 0.0f;
    if (crop_percent > 0.4f) crop_percent = 0.4f;
    st->crop_percent = crop_percent;
}

void wb_stabilize2_reset(void *s) {
    if (!s) return;
    wb_stabilize2_inst *st = (wb_stabilize2_inst *)s;
    st->cum_dx = 0;
    st->cum_y = 0;
    st->smooth_dx = 0;
    st->smooth_y = 0;
    st->initialized = 0;
    st->frame_count = 0;
    memset(st->prev_gray, 0, st->width * st->height * sizeof(float));
    memset(st->curr_gray, 0, st->width * st->height * sizeof(float));
}