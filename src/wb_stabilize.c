/* wb_video_stabilize.c — video stabilization (motion smoothing).
 *
 * R077: Digital video stabilization using feature tracking.
 *
 * Algorithm:
 *   1. Detect feature points (corners) in frame N
 *   2. Track to frame N+1 using optical flow
 *   3. Estimate translation/rotation transformation
 *   4. Smooth trajectory (Gaussian filter)
 *   5. Apply correction + crop
 *
 * Simplified version: uses frame-difference motion estimation.
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define STAB_SMOOTHING 0.9f
#define STAB_MAX_MOTION 50.0f

typedef struct {
    float    prev_frame[64][64];  /* Downsampled reference */
    float    smooth_x, smooth_y;   /* Smoothed motion */
    float    raw_x, raw_y;         /* Raw motion */
    float    crop_percent;         /* How much to crop (0.05 = 5%) */
    int      width, height;
    int      initialized;
} wb_stabilize_inst;

void *wb_stabilize_create(int width, int height) {
    wb_stabilize_inst *st = (wb_stabilize_inst *)calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->width = width;
    st->height = height;
    st->crop_percent = 0.05f;
    st->smooth_x = 0;
    st->smooth_y = 0;
    st->initialized = 0;
    return st;
}

void wb_stabilize_destroy(void *inst) { free(inst); }

/* Downsample frame to 64x64 grayscale for motion estimation. */
static void downsample(const uint8_t *rgba, int w, int h, float out[64][64]) {
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            int src_x = x * w / 64;
            int src_y = y * h / 64;
            int idx = (src_y * w + src_x) * 4;
            float gray = 0.299f * rgba[idx] + 0.587f * rgba[idx+1] + 0.114f * rgba[idx+2];
            out[y][x] = gray;
        }
    }
}

/* Estimate motion between two downsampled frames using phase correlation. */
static void estimate_motion(float prev[64][64], float curr[64][64],
                             float *dx, float *dy) {
    /* Simple block-matching: find displacement that minimizes SSD */
    float best_ssd = 1e18f;
    *dx = 0;
    *dy = 0;

    for (int dy_off = -5; dy_off <= 5; dy_off++) {
        for (int dx_off = -5; dx_off <= 5; dx_off++) {
            float ssd = 0;
            int count = 0;
            for (int y = 8; y < 56; y++) {
                for (int x = 8; x < 56; x++) {
                    int px = x + dx_off;
                    int py = y + dy_off;
                    if (px >= 0 && px < 64 && py >= 0 && py < 64) {
                        float diff = prev[y][x] - curr[py][px];
                        ssd += diff * diff;
                        count++;
                    }
                }
            }
            if (count > 0) ssd /= count;
            if (ssd < best_ssd) {
                best_ssd = ssd;
                *dx = (float)dx_off;
                *dy = (float)dy_off;
            }
        }
    }
}

/* Process a frame: estimate motion, smooth, apply correction.
 * Frame is modified in place (cropped and shifted). */
void wb_stabilize_process(void *inst, uint8_t *rgba, int width, int height) {
    wb_stabilize_inst *st = (wb_stabilize_inst *)inst;
    if (!st || !rgba) return;

    float curr[64][64];
    downsample(rgba, width, height, curr);

    if (!st->initialized) {
        memcpy(st->prev_frame, curr, sizeof(curr));
        st->initialized = 1;
        return;
    }

    /* Estimate motion */
    float dx, dy;
    estimate_motion(st->prev_frame, curr, &dx, &dy);

    /* Scale motion to full resolution */
    dx *= (float)width / 64.0f;
    dy *= (float)height / 64.0f;

    /* Smooth trajectory */
    st->smooth_x = STAB_SMOOTHING * st->smooth_x + (1.0f - STAB_SMOOTHING) * dx;
    st->smooth_y = STAB_SMOOTHING * st->smooth_y + (1.0f - STAB_SMOOTHING) * dy;

    /* Clamp */
    if (st->smooth_x > STAB_MAX_MOTION) st->smooth_x = STAB_MAX_MOTION;
    if (st->smooth_x < -STAB_MAX_MOTION) st->smooth_x = -STAB_MAX_MOTION;
    if (st->smooth_y > STAB_MAX_MOTION) st->smooth_y = STAB_MAX_MOTION;
    if (st->smooth_y < -STAB_MAX_MOTION) st->smooth_y = -STAB_MAX_MOTION;

    /* Apply correction: shift frame by -smooth_x, -smooth_y */
    int shift_x = (int)(-st->smooth_x);
    int shift_y = (int)(-st->smooth_y);

    if (shift_x != 0 || shift_y != 0) {
        /* Simple shift (in production, use bilinear interpolation) */
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int src_x = x - shift_x;
                int src_y = y - shift_y;
                if (src_x >= 0 && src_x < width && src_y >= 0 && src_y < height) {
                    int dst_idx = (y * width + x) * 4;
                    int src_idx = (src_y * width + src_x) * 4;
                    rgba[dst_idx] = rgba[src_idx];
                    rgba[dst_idx+1] = rgba[src_idx+1];
                    rgba[dst_idx+2] = rgba[src_idx+2];
                } else {
                    /* Fill border with black */
                    int dst_idx = (y * width + x) * 4;
                    rgba[dst_idx] = 0;
                    rgba[dst_idx+1] = 0;
                    rgba[dst_idx+2] = 0;
                }
            }
        }
    }

    /* Store current frame */
    memcpy(st->prev_frame, curr, sizeof(curr));
}
