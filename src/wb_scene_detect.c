/* wb_scene_detect.c — scene/shot boundary detection.
 *
 * R077: Auto-detect cuts and scene changes in video.
 *
 * Algorithm:
 *   1. Compute histogram difference between consecutive frames
 *   2. Detect sudden changes (threshold)
 *   3. Optional: motion-based filtering (ignore camera pan)
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define MAX_SCENES 512
#define HIST_BINS 64

typedef struct {
    uint32_t frame_number;
    float    timestamp;
    float    confidence;
} scene_cut_t;

typedef struct {
    scene_cut_t cuts[MAX_SCENES];
    int         num_cuts;
    float       threshold;      /* Histogram diff threshold (0..1) */
    int         min_frames;     /* Min frames between cuts */
    uint8_t     prev_hist[HIST_BINS * 3]; /* Previous frame histogram */
    int         frame_count;
    int         has_prev;
} wb_scene_detect_inst;

void *wb_scene_detect_create(void) {
    wb_scene_detect_inst *sd = (wb_scene_detect_inst *)calloc(1, sizeof(*sd));
    if (!sd) return NULL;
    sd->threshold = 0.35f;
    sd->min_frames = 12;  /* At least 0.4 sec between cuts @ 30fps */
    return sd;
}

void wb_scene_detect_destroy(void *inst) { free(inst); }

void wb_scene_detect_set(void *inst, int param, float v) {
    wb_scene_detect_inst *sd = (wb_scene_detect_inst *)inst;
    if (!sd) return;
    switch (param) {
    case 0: sd->threshold = v < 0.05f ? 0.05f : (v > 1.0f ? 1.0f : v); break;
    case 1: sd->min_frames = (int)v > 0 ? (int)v : 1; break;
    default: break;
    }
}

/* Compute a simple color histogram from an RGBA frame. */
static void compute_histogram(const uint8_t *rgba, int width, int height,
                                uint8_t *hist) {
    memset(hist, 0, HIST_BINS * 3);

    int step = 4;  /* Sample every 4th pixel for speed */
    int count = 0;

    for (int y = 0; y < height; y += step) {
        for (int x = 0; x < width; x += step) {
            int idx = (y * width + x) * 4;
            int r_bin = rgba[idx] * HIST_BINS / 256;
            int g_bin = rgba[idx + 1] * HIST_BINS / 256;
            int b_bin = rgba[idx + 2] * HIST_BINS / 256;
            hist[r_bin]++;
            hist[HIST_BINS + g_bin]++;
            hist[HIST_BINS * 2 + b_bin]++;
            count++;
        }
    }

    /* Normalize */
    if (count > 0) {
        for (int i = 0; i < HIST_BINS * 3; i++) {
            hist[i] = (uint8_t)((float)hist[i] * 255.0f / (float)count);
        }
    }
}

/* Compute histogram difference (0..1). */
static float hist_diff(const uint8_t *h1, const uint8_t *h2) {
    float diff = 0;
    for (int i = 0; i < HIST_BINS * 3; i++) {
        diff += abs((int)h1[i] - (int)h2[i]);
    }
    return diff / (float)(HIST_BINS * 3 * 255);
}

/* Process a frame. Returns 1 if scene cut detected, 0 otherwise. */
int wb_scene_detect_frame(wb_scene_detect_inst *sd, const uint8_t *rgba,
                            int width, int height, float timestamp) {
    if (!sd || !rgba) return 0;

    sd->frame_count++;

    uint8_t curr_hist[HIST_BINS * 3];
    compute_histogram(rgba, width, height, curr_hist);

    if (!sd->has_prev) {
        memcpy(sd->prev_hist, curr_hist, sizeof(curr_hist));
        sd->has_prev = 1;
        return 0;
    }

    float diff = hist_diff(sd->prev_hist, curr_hist);
    memcpy(sd->prev_hist, curr_hist, sizeof(curr_hist));

    /* Check threshold and minimum frame distance */
    if (diff > sd->threshold &&
        sd->frame_count > sd->min_frames &&
        sd->num_cuts < MAX_SCENES) {

        scene_cut_t *cut = &sd->cuts[sd->num_cuts++];
        cut->frame_number = sd->frame_count;
        cut->timestamp = timestamp;
        cut->confidence = diff;
        sd->frame_count = 0;  /* Reset min frame counter */
        return 1;
    }

    return 0;
}

/* Get detected cuts. */
const scene_cut_t* wb_scene_detect_get_cuts(wb_scene_detect_inst *sd, int *out_num) {
    if (!sd) return NULL;
    if (out_num) *out_num = sd->num_cuts;
    return sd->cuts;
}
