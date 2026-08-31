/* tests/test_autoreframe.c — test auto-reframe feature. */
#include <stdio.h>
#include <string.h>
#include "wbus.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

int main(void) {
    int src_w = 1920, src_h = 1080, dst_w = 1080, dst_h = 1920;
    wb_autoreframe *ar = (wb_autoreframe *)calloc(1, sizeof(wb_autoreframe));
    int rc = wb_autoreframe_init(ar, src_w, src_h, dst_w, dst_h);
    CHECK(rc == 0);

    /* Create test frame: white rectangle on black */
    uint8_t *frame = (uint8_t *)calloc(src_w * src_h * 4, 1);
    uint8_t *out = (uint8_t *)calloc(dst_w * dst_h * 4, 1);
    CHECK(frame && out);

    /* Draw white rect at center-left */
    for (int y = 400; y < 600; y++)
        for (int x = 200; x < 400; x++) {
            int idx = (y * src_w + x) * 4;
            frame[idx] = 255; frame[idx+1] = 255; frame[idx+2] = 255; frame[idx+3] = 255;
        }

    /* 1. Center mode */
    wb_autoreframe_set_mode(ar, 0);
    rc = wb_autoreframe_process(ar, frame, src_w, src_h, out, dst_w, dst_h);
    CHECK(rc == 0);

    /* 2. Output has non-zero pixels */
    int nonzero = 0;
    for (int i = 0; i < dst_w * dst_h * 4; i += 4)
        if (out[i] > 0) { nonzero = 1; break; }
    CHECK(nonzero);

    /* 3. Subject tracking */
    wb_autoreframe_set_subject(ar, 200, 400, 200, 200);
    wb_autoreframe_set_mode(ar, 1);
    rc = wb_autoreframe_process(ar, frame, src_w, src_h, out, dst_w, dst_h);
    CHECK(rc == 0);

    /* 4. Rule of thirds */
    wb_autoreframe_set_mode(ar, 3);
    rc = wb_autoreframe_process(ar, frame, src_w, src_h, out, dst_w, dst_h);
    CHECK(rc == 0);

    /* 5. Smoothing */
    wb_autoreframe_set_smoothing(ar, 0.9f);
    rc = wb_autoreframe_process(ar, frame, src_w, src_h, out, dst_w, dst_h);
    CHECK(rc == 0);

    free(frame); free(out); free(ar);
    printf("\nAuto-Reframe: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}
