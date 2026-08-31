/* tests/test_color_grading.c — test color grading feature. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wbus.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

int main(void) {
    void *cg = wb_color_grading_create(1920, 1080);
    CHECK(cg != NULL);

    /* Create test image: gradient */
    int w = 100, h = 100;
    uint8_t *rgba = (uint8_t *)calloc(w * h * 4, 1);
    for (int i = 0; i < w * h; i++) {
        rgba[i*4] = (uint8_t)(i % 256);
        rgba[i*4+1] = (uint8_t)((i * 2) % 256);
        rgba[i*4+2] = (uint8_t)((i * 3) % 256);
        rgba[i*4+3] = 255;
    }

    /* 1. Process frame */
    int rc = wb_color_grading_process(cg, rgba, w, h);
    CHECK(rc == 0);

    /* 2. Lift affects shadows */
    uint8_t *rgba2 = (uint8_t *)malloc(w * h * 4);
    memcpy(rgba2, rgba, w * h * 4);
    wb_color_grading_set_lift(cg, 1.2f, 1.0f, 0.8f);
    wb_color_grading_process(cg, rgba2, w, h);
    int changed = 0;
    for (int i = 0; i < w * h * 4; i += 4) {
        if (rgba[i] != rgba2[i]) { changed = 1; break; }
    }
    CHECK(changed);

    /* 3. Saturation changes color */
    memcpy(rgba2, rgba, w * h * 4);
    wb_color_grading_set_lift(cg, 1.0f, 1.0f, 1.0f); /* reset */
    wb_color_grading_set_saturation(cg, 2.0f);
    wb_color_grading_process(cg, rgba2, w, h);
    int sat_changed = 0;
    for (int i = 0; i < w * h * 4; i += 4) {
        if (rgba[i+1] != rgba2[i+1]) { sat_changed = 1; break; }
    }
    CHECK(sat_changed);

    /* 4. Contrast affects output */
    memcpy(rgba2, rgba, w * h * 4);
    wb_color_grading_set_saturation(cg, 1.0f);
    wb_color_grading_set_contrast(cg, 1.5f);
    wb_color_grading_process(cg, rgba2, w, h);
    int con_changed = 0;
    for (int i = 0; i < w * h * 4; i += 4) {
        if (rgba[i+2] != rgba2[i+2]) { con_changed = 1; break; }
    }
    CHECK(con_changed);

    /* 5. Temperature affects output */
    memcpy(rgba2, rgba, w * h * 4);
    wb_color_grading_set_contrast(cg, 1.0f);
    wb_color_grading_set_temperature(cg, 0.5f);
    wb_color_grading_process(cg, rgba2, w, h);
    int temp_changed = 0;
    for (int i = 0; i < w * h * 4; i += 4) {
        if (rgba[i] != rgba2[i] || rgba[i+2] != rgba2[i+2]) { temp_changed = 1; break; }
    }
    CHECK(temp_changed);

    /* 6. Output valid (all in 0-255) */
    int valid = 1;
    for (int i = 0; i < w * h * 4; i++) {
        if (rgba2[i] > 255) valid = 0;
    }
    CHECK(valid);

    free(rgba);
    free(rgba2);
    wb_color_grading_destroy(cg);

    printf("\nColor Grading: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}
