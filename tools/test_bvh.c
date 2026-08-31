/* test_bvh.c — verify BVH parser (R081) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_bvh.h"

#define MAX_CH 768
#define MAX_JOINTS 128

int main(int argc, char **argv) {
    int pass = 1;

    const char *test_file = "/Users/waefrebeorn/Documents/big-mac/ref/amc2bvh/test.bvh";
    if (argc > 1) test_file = argv[1];

    printf("=== BVH Parser Test ===\n");
    printf("Loading: %s\n", test_file);

    wb_bvh *b = wb_bvh_load(test_file);
    if (!b) {
        printf("FAIL: could not load BVH: %s\n", wb_bvh_error_string());
        return 1;
    }

    int joints = wb_bvh_joint_count(b);
    int frames = wb_bvh_frame_count(b);
    double dur = wb_bvh_duration(b);
    double ft = wb_bvh_frame_time(b);

    printf("Joints: %d\n", joints);
    printf("Frames: %d\n", frames);
    printf("Frame time: %.4f s\n", ft);
    printf("Duration: %.2f s\n", dur);

    if (joints < 10) { printf("FAIL: too few joints (%d)\n", joints); pass = 0; }
    if (frames < 100) { printf("FAIL: too few frames (%d)\n", frames); pass = 0; }

    /* Print joint hierarchy */
    const wb_bvh_joint *js = wb_bvh_get_joints(b);
    printf("\nJoint hierarchy:\n");
    for (int j = 0; j < joints && j < 10; j++) {
        printf("  [%d] %-20s parent=%d offset=(%.2f,%.2f,%.2f) ch=%d%s\n",
               j, js[j].name, js[j].parent,
               js[j].offset[0], js[j].offset[1], js[j].offset[2],
               js[j].n_channels,
               js[j].is_site ? " (site)" : "");
    }
    if (joints > 10) printf("  ... (%d more)\n", joints - 10);

    /* Frame sampling buffer */
    float frame_data[MAX_CH];
    memset(frame_data, 0, sizeof(frame_data));

    /* Sample at t=0 */
    if (wb_bvh_sample(b, 0.0, frame_data) == 0) {
        printf("\nFrame 0 sampled OK\n");
    } else {
        printf("\nFAIL: frame 0 sample\n");
        pass = 0;
    }

    /* Sample at mid-duration */
    double mid = dur / 2.0;
    if (wb_bvh_sample(b, mid, frame_data) == 0) {
        printf("Mid-frame (t=%.2f) sampled OK\n", mid);
    } else {
        printf("FAIL: mid-frame sample\n");
        pass = 0;
    }

    /* Test 2D position computation */
    float positions[MAX_JOINTS * 2];
    memset(positions, 0, sizeof(positions));
    int n = wb_bvh_compute_positions_2d(b, frame_data, positions, joints, 2.0f, 427.0f, 240.0f);
    printf("\n2D positions computed: %d joints\n", n);

    /* Check positions are reasonable (within screen bounds roughly) */
    int in_bounds = 0;
    for (int j = 0; j < n; j++) {
        float x = positions[j*2+0], y = positions[j*2+1];
        if (x >= -100 && x <= 1000 && y >= -100 && y <= 600) in_bounds++;
    }
    printf("Joints in reasonable bounds: %d/%d\n", in_bounds, n);
    if (in_bounds < n / 2) {
        printf("WARNING: many joints out of bounds (scale may need adjustment)\n");
    }

    /* Print first few positions */
    for (int j = 0; j < n && j < 8; j++) {
        printf("  [%d] (%.1f, %.1f)\n", j, positions[j*2+0], positions[j*2+1]);
    }

    /* Test rendering to a small buffer */
    int rw = 854, rh = 480;
    uint8_t *rgba = calloc(rw * rh * 4, 1);
    memset(frame_data, 0, sizeof(frame_data));
    wb_bvh_sample(b, 0.0, frame_data);
    if (wb_bvh_render_skeleton(b, frame_data, rgba, rw, rh, 2.0f, 427.0f, 240.0f,
                                0, 255, 0, 3.0f) == 0) {
        /* Count non-black pixels */
        int drawn = 0;
        for (int p = 0; p < rw * rh; p++) {
            if (rgba[p*4+3] > 0) drawn++;
        }
        printf("\nSkeleton rendered: %d pixels drawn\n", drawn);
        if (drawn < 100) {
            printf("FAIL: too few pixels drawn\n");
            pass = 0;
        }
    } else {
        printf("FAIL: skeleton render\n");
        pass = 0;
    }

    /* Test animation: sample multiple frames */
    printf("\nAnimation sampling test:\n");
    float *f0 = malloc(MAX_CH * sizeof(float));
    float *f1 = malloc(MAX_CH * sizeof(float));
    wb_bvh_sample(b, 0.0, f0);
    wb_bvh_sample(b, dur * 0.25, f1);
    /* Compare root position channel */
    int root_joint = 0;
    const wb_bvh_joint *rj = &js[root_joint];
    if (rj->channel_indices[BVH_CH_XPOS] >= 0) {
        int ci = rj->channel_indices[BVH_CH_XPOS];
        printf("  Root X: t=0 -> %.2f, t=%.2f -> %.2f\n",
               f0[ci], dur*0.25, f1[ci]);
    } else {
        printf("  Root has no X position channel (rotation-only root)\n");
    }
    free(f0);
    free(f1);

    free(rgba);
    wb_bvh_free(b);

    printf("\n%s\n", pass ? "ALL PASS" : "SOME FAIL");
    return pass ? 0 : 1;
}
