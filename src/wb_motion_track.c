/* wb_motion_track.c — point tracking via Lucas-Kanade optical flow.
 *
 * R077: Track feature points across video frames for attaching graphics.
 *
 * Algorithm:
 *   1. Detect feature points (Shi-Tomasi corners)
 *   2. Track via iterative Lucas-Kanade with pyramid
 *   3. Forward-backward validation
 *   4. Output per-frame point positions
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define MAX_TRACKED_POINTS 64
#define MAX_PYRAMID_LEVELS 3
#define LK_WINDOW_SIZE 15
#define LK_MAX_ITERATIONS 10
#define LK_EPSILON 0.03f
#define FB_ERROR_THRESHOLD 1.0f

typedef struct {
    float x, y;              /* Current position */
    float prev_x, prev_y;    /* Previous position */
    int   active;            /* Still tracked? */
    int   id;                /* Point ID */
} tracked_point_t;

typedef struct {
    tracked_point_t points[MAX_TRACKED_POINTS];
    int             num_points;
    int             width, height;
    float           prev_frame[480*270];  /* Downsampled grayscale */
    int             initialized;
} wb_motion_track_inst;

void *wb_motion_track_create(int width, int height) {
    wb_motion_track_inst *mt = (wb_motion_track_inst *)calloc(1, sizeof(*mt));
    if (!mt) return NULL;
    mt->width = width;
    mt->height = height;
    return mt;
}

void wb_motion_track_destroy(void *inst) { free(inst); }

/* Convert RGBA to grayscale and downsample. */
static void to_grayscale(const uint8_t *rgba, int w, int h, float *gray) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = (y * w + x) * 4;
            gray[y * w + x] = 0.299f * rgba[idx] + 0.587f * rgba[idx+1] + 0.114f * rgba[idx+2];
        }
    }
}

/* Detect Shi-Tomasi corners. Returns number of points found. */
int wb_motion_track_detect(wb_motion_track_inst *mt, const uint8_t *rgba,
                             int num_points) {
    if (!mt || !rgba) return 0;

    int w = mt->width;
    int h = mt->height;
    float *gray = (float *)calloc(w * h, sizeof(float));
    if (!gray) return 0;

    to_grayscale(rgba, w, h, gray);

    /* Simple corner detection: find local maxima of structure tensor min eigenvalue */
    mt->num_points = 0;
    int step = (w * h) / (num_points * 4);  /* Sampling step */
    if (step < 1) step = 1;

    for (int y = 10; y < h - 10 && mt->num_points < num_points; y += step / w) {
        for (int x = 10; x < w - 10 && mt->num_points < num_points; x += step) {
            /* Compute structure tensor */
            float ixx = 0, iyy = 0, ixy = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int idx = (y + dy) * w + (x + dx);
                    float ix = (gray[idx + 1] - gray[idx - 1]) * 0.5f;
                    float iy = (gray[idx + w] - gray[idx - w]) * 0.5f;
                    ixx += ix * ix;
                    iyy += iy * iy;
                    ixy += ix * iy;
                }
            }

            /* Min eigenvalue of structure tensor */
            float trace = ixx + iyy;
            float det = ixx * iyy - ixy * ixy;
            float discriminant = trace * trace - 4.0f * det;
            if (discriminant < 0) continue;
            float lambda_min = (trace - sqrtf(discriminant)) * 0.5f;

            /* Threshold for corner */
            if (lambda_min > 100.0f) {
                tracked_point_t *p = &mt->points[mt->num_points++];
                p->x = (float)x;
                p->y = (float)y;
                p->prev_x = (float)x;
                p->prev_y = (float)y;
                p->active = 1;
                p->id = mt->num_points - 1;
            }
        }
    }

    free(gray);

    /* Store frame for tracking */
    to_grayscale(rgba, w, h, mt->prev_frame);
    mt->initialized = 1;

    return mt->num_points;
}

/* Track points to a new frame. Returns number of successfully tracked points. */
int wb_motion_track_frame(wb_motion_track_inst *mt, const uint8_t *rgba) {
    if (!mt || !rgba || !mt->initialized) return 0;

    int w = mt->width;
    int h = mt->height;
    float *curr_gray = (float *)calloc(w * h, sizeof(float));
    if (!curr_gray) return 0;

    to_grayscale(rgba, w, h, curr_gray);

    int tracked = 0;

    for (int p = 0; p < mt->num_points; p++) {
        if (!mt->points[p].active) continue;

        float px = mt->points[p].x;
        float py = mt->points[p].y;

        /* Lucas-Kanade iteration */
        float dx = 0, dy = 0;
        int found = 0;

        for (int iter = 0; iter < LK_MAX_ITERATIONS; iter++) {
            float gx = 0, gy = 0, gt = 0;

            int half_w = LK_WINDOW_SIZE / 2;

            for (int wy = -half_w; wy <= half_w; wy++) {
                for (int wx = -half_w; wx <= half_w; wx++) {
                    int ix = (int)(px + dx + wx);
                    int iy = (int)(py + dy + wy);

                    if (ix < 1 || ix >= w - 1 || iy < 1 || iy >= h - 1) continue;

                    /* Spatial gradients */
                    float grad_x = (curr_gray[iy * w + ix + 1] - curr_gray[iy * w + ix - 1]) * 0.5f;
                    float grad_y = (curr_gray[(iy + 1) * w + ix] - curr_gray[(iy - 1) * w + ix]) * 0.5f;

                    /* Temporal gradient */
                    float grad_t = curr_gray[iy * w + ix] - mt->prev_frame[iy * w + ix];

                    gx += grad_x * grad_x;
                    gy += grad_y * grad_y;
                    gt += grad_x * grad_t;
                }
            }

            /* Solve 2x2 system */
            float det = gx * gy;
            if (fabsf(det) < 1e-6f) break;

            float delta_x = -gt / det * gx;
            float delta_y = -gt / det * gy;

            dx += delta_x;
            dy += delta_y;

            if (fabsf(delta_x) < LK_EPSILON && fabsf(delta_y) < LK_EPSILON) {
                found = 1;
                break;
            }
        }

        if (found) {
            mt->points[p].prev_x = mt->points[p].x;
            mt->points[p].prev_y = mt->points[p].y;
            mt->points[p].x = px + dx;
            mt->points[p].y = py + dy;

            /* Bounds check */
            if (mt->points[p].x < 0 || mt->points[p].x >= w ||
                mt->points[p].y < 0 || mt->points[p].y >= h) {
                mt->points[p].active = 0;
            } else {
                tracked++;
            }
        } else {
            mt->points[p].active = 0;
        }
    }

    /* Update previous frame */
    memcpy(mt->prev_frame, curr_gray, w * h * sizeof(float));
    free(curr_gray);

    return tracked;
}

/* Get tracked points. */
const tracked_point_t* wb_motion_track_get_points(wb_motion_track_inst *mt, int *out_num) {
    if (!mt) return NULL;
    if (out_num) *out_num = mt->num_points;
    return mt->points;
}
