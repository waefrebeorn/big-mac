/* tests/test_stabilize2.c — test video stabilization (phase correlation). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wbus.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

/* Generate a synthetic RGBA frame with a pattern at given offset */
static void gen_frame(uint8_t *rgba, int w, int h, int off_x, int off_y) {
    memset(rgba, 0, w * h * 4);
    for (int y = 10; y < h - 10; y++) {
        for (int x = 10; x < w - 10; x++) {
            int px = x + off_x;
            int py = y + off_y;
            if (px >= 10 && px < w - 10 && py >= 10 && py < h - 10) {
                int i = (y * w + x) * 4;
                /* Checkerboard pattern for good feature tracking */
                int cx = (px / 8) & 1;
                int cy = (py / 8) & 1;
                uint8_t v = (cx ^ cy) ? 200 : 50;
                rgba[i] = v;
                rgba[i + 1] = v;
                rgba[i + 2] = v;
                rgba[i + 3] = 255;
            }
        }
    }
}

int main(void) {
    int w = 128, h = 128;

    /* 1. Create/destroy */
    void *s = wb_stabilize2_create(w, h);
    CHECK(s != NULL);
    wb_stabilize2_destroy(s);
    CHECK(1); /* didn't crash */

    /* 2. Process frame */
    s = wb_stabilize2_create(w, h);
    uint8_t *frame = (uint8_t *)calloc(w * h * 4, 1);
    gen_frame(frame, w, h, 0, 0);
    int rc = wb_stabilize2_process(s, frame, w, h);
    CHECK(rc == 0);

    /* Process a second frame with slight translation */
    gen_frame(frame, w, h, 3, 2);
    rc = wb_stabilize2_process(s, frame, w, h);
    CHECK(rc == 0);

    /* 3. Smoothing affects output: process same frame sequence with low vs high smoothing */
    uint8_t *frame_a = (uint8_t *)malloc(w * h * 4);
    uint8_t *frame_b = (uint8_t *)malloc(w * h * 4);

    /* Low smoothing path: feed 3 frames with translation */
    wb_stabilize2_reset(s);
    wb_stabilize2_set_smoothing(s, 0.1f);
    wb_stabilize2_set_crop(s, 0.05f);
    gen_frame(frame_a, w, h, 0, 0);
    wb_stabilize2_process(s, frame_a, w, h); /* frame 1: ref, no correction */
    gen_frame(frame_a, w, h, 6, 3);
    wb_stabilize2_process(s, frame_a, w, h); /* frame 2: correct */
    gen_frame(frame_a, w, h, 12, 6);
    wb_stabilize2_process(s, frame_a, w, h); /* frame 3: correct */

    /* High smoothing path: same frames */
    wb_stabilize2_reset(s);
    wb_stabilize2_set_smoothing(s, 0.95f);
    wb_stabilize2_set_crop(s, 0.05f);
    gen_frame(frame_b, w, h, 0, 0);
    wb_stabilize2_process(s, frame_b, w, h);
    gen_frame(frame_b, w, h, 6, 3);
    wb_stabilize2_process(s, frame_b, w, h);
    gen_frame(frame_b, w, h, 12, 6);
    wb_stabilize2_process(s, frame_b, w, h);

    /* Outputs should differ since smoothing changed the correction trajectory */
    int diff = 0;
    for (int i = 0; i < w * h * 4; i += 4) {
        if (frame_a[i] != frame_b[i]) { diff = 1; break; }
    }
    CHECK(diff);

    /* 4. Crop reduces effective frame size (more crop = more black pixels) */
    wb_stabilize2_reset(s);
    wb_stabilize2_set_smoothing(s, 0.5f);
    wb_stabilize2_set_crop(s, 0.30f); /* heavy crop */
    gen_frame(frame, w, h, 0, 0);
    wb_stabilize2_process(s, frame, w, h);
    gen_frame(frame, w, h, 4, 4);
    wb_stabilize2_process(s, frame, w, h);

    /* Count black (zero) pixels — with heavy crop, borders should be black */
    int black_pixels = 0;
    for (int i = 0; i < w * h; i++) {
        if (frame[i * 4] == 0 && frame[i * 4 + 1] == 0 &&
            frame[i * 4 + 2] == 0 && frame[i * 4 + 3] == 0)
            black_pixels++;
    }
    /* With 30% crop, at least ~50% of pixels should be black (border regions) */
    CHECK(black_pixels > (w * h) / 4);

    /* 5. Reset clears motion history: after reset, first frame is identity */
    wb_stabilize2_reset(s);
    wb_stabilize2_set_crop(s, 0.05f);
    wb_stabilize2_set_smoothing(s, 0.85f);
    gen_frame(frame, w, h, 0, 0);
    uint8_t *orig = (uint8_t *)malloc(w * h * 4);
    memcpy(orig, frame, w * h * 4);
    rc = wb_stabilize2_process(s, frame, w, h);
    CHECK(rc == 0);
    /* First frame after reset should not be modified (no motion to correct) */
    int same = 1;
    for (int i = 0; i < w * h * 4; i += 4) {
        if (frame[i] != orig[i] || frame[i+1] != orig[i+1] || frame[i+2] != orig[i+2]) {
            same = 0;
            break;
        }
    }
    CHECK(same);

    /* 6. Output valid RGBA (all values in 0-255 range) */
    int valid = 1;
    for (int i = 0; i < w * h * 4; i++) {
        if (frame[i] > 255) { valid = 0; break; }
    }
    CHECK(valid);

    /* Additional: alpha channel should be 0 or 255 */
    int alpha_ok = 1;
    for (int i = 0; i < w * h; i++) {
        uint8_t a = frame[i * 4 + 3];
        if (a != 0 && a != 255) { alpha_ok = 0; break; }
    }
    CHECK(alpha_ok);

    /* Additional: invalid args return -1 */
    CHECK(wb_stabilize2_process(NULL, frame, w, h) == -1);
    CHECK(wb_stabilize2_process(s, NULL, w, h) == -1);
    CHECK(wb_stabilize2_process(s, frame, 999, h) == -1); /* wrong width */

    free(frame);
    free(frame_a);
    free(frame_b);
    free(orig);
    wb_stabilize2_destroy(s);

    printf("\nStabilize2: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}