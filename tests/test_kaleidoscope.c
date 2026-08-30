/* tests/test_kaleidoscope.c — headless test of kaleidoscope effect */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus.h"

void *wb_kaleidoscope_create(int width, int height);
void  wb_kaleidoscope_destroy(void *inst);
void  wb_kaleidoscope_set_segments(void *inst, int n);
void  wb_kaleidoscope_set_rotation_speed(void *inst, float speed);
void  wb_kaleidoscope_set_mirror(void *inst, int on);
void  wb_kaleidoscope_set_zoom(void *inst, float zoom);
void  wb_kaleidoscope_process(void *inst, uint8_t *buf, int width, int height);

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

int main(void) {
    printf("=== Kaleidoscope Test ===\n");
    int w = 64, h = 64;

    void *k = wb_kaleidoscope_create(w, h);
    CHECK(k != NULL);

    /* Create a test pattern */
    uint8_t *buf = (uint8_t *)calloc(w * h * 4, 1);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int i = (y * w + x) * 4;
            buf[i] = (uint8_t)(x * 4);     /* R gradient */
            buf[i + 1] = (uint8_t)(y * 4); /* G gradient */
            buf[i + 2] = 128;              /* B constant */
            buf[i + 3] = 255;              /* A */
        }
    }

    /* Test 1: Process doesn't crash */
    wb_kaleidoscope_process(k, buf, w, h);
    CHECK(1); /* If we got here, no crash */

    /* Test 2: Output is non-zero */
    int nonzero = 0;
    for (int i = 0; i < w * h * 4; i += 4) {
        if (buf[i] || buf[i + 1] || buf[i + 2]) { nonzero = 1; break; }
    }
    CHECK(nonzero);

    /* Test 3: Different segment counts */
    for (int seg = 2; seg <= 12; seg += 2) {
        wb_kaleidoscope_set_segments(k, seg);
        wb_kaleidoscope_process(k, buf, w, h);
    }
    CHECK(1); /* No crash across all segment counts */

    /* Test 4: Mirror toggle */
    wb_kaleidoscope_set_mirror(k, 1);
    wb_kaleidoscope_process(k, buf, w, h);
    wb_kaleidoscope_set_mirror(k, 0);
    wb_kaleidoscope_process(k, buf, w, h);
    CHECK(1);

    /* Test 5: Zoom range */
    wb_kaleidoscope_set_zoom(k, 0.5f);
    wb_kaleidoscope_process(k, buf, w, h);
    wb_kaleidoscope_set_zoom(k, 2.0f);
    wb_kaleidoscope_process(k, buf, w, h);
    CHECK(1);

    /* Test 6: Animation (multiple frames) */
    wb_kaleidoscope_set_segments(k, 8);
    wb_kaleidoscope_set_rotation_speed(k, 0.1f);
    for (int frame = 0; frame < 10; frame++) {
        wb_kaleidoscope_process(k, buf, w, h);
    }
    CHECK(1);

    free(buf);
    wb_kaleidoscope_destroy(k);

    printf("\nKaleidoscope: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}
