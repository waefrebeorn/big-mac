/* tests/test_lottie.c — test Lottie renderer feature. */
#include <stdio.h>
#include <string.h>
#include "wbus.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

int main(void) {
    void *lottie = wb_lottie_create();
    CHECK(lottie != NULL);

    /* Simple Lottie JSON: one rectangle layer */
    const char *json = "{"
        "\"v\":\"5.7.0\",\"fr\":30,\"ip\":0,\"op\":30,\"w\":100,\"h\":100,"
        "\"layers\":["
            "{\"ty\":0,\"nm\":\"rect\",\"ks\":{\"o\":{\"a\":0,\"k\":100},"
            "\"p\":{\"a\":0,\"k\":[50,50]},\"s\":{\"a\":0,\"k\":[100,100]}},"
            "\"shapes\":[]}"
        "]"
    "}";

    /* 1. Load JSON */
    int nl = wb_lottie_load_json(lottie, json, (int)strlen(json));
    CHECK(nl > 0);

    /* 2. Duration and FPS */
    float dur = wb_lottie_get_duration(lottie);
    CHECK(dur > 0);
    int fps = wb_lottie_get_fps(lottie);
    CHECK(fps == 30);

    /* 3. Render frame */
    uint8_t rgba[100*100*4];
    int rc = wb_lottie_render_frame(lottie, rgba, 100, 100, 0.0f);
    CHECK(rc == 0);

    /* 4. Output has non-zero pixels */
    int nonzero = 0;
    for (int i = 0; i < 100*100*4; i += 4)
        if (rgba[i+3] > 0) { nonzero = 1; break; }
    CHECK(nonzero);

    /* 5. Render at different time */
    rc = wb_lottie_render_frame(lottie, rgba, 100, 100, 0.5f);
    CHECK(rc == 0);

    wb_lottie_destroy(lottie);
    printf("\nLottie: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}
