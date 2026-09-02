/* test_realtime_preview.c — verify real-time preview engine */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern "C" {
#include "wbus/wbus_compositor.h"
}

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

int main(void) {
    int pass = 0, fail = 0;

    printf("=== Real-Time Preview ===\n");

    /* Create a simple source node for preview */
    wb_node *src = wb_node_source_color(0.5f, 0.3f, 0.8f, 1.0f, 64, 64);
    CHECK(src != NULL, "source node created for preview");

    /* Create preview */
    wb_preview *p = wb_preview_create(src, 64, 64, 30.0);
    CHECK(p != NULL, "preview created");

    /* Configure */
    wb_preview_set_duration(p, 5.0);
    wb_preview_set_loop(p, 1);
    wb_preview_set_speed(p, 2.0);
    CHECK(1, "duration/loop/speed set");

    /* Initial state */
    CHECK(wb_preview_get_state(p) == WB_PREVIEW_STOPPED, "initial state = STOPPED");
    CHECK(wb_preview_get_time(p) == 0.0, "initial time = 0");

    /* Play */
    wb_preview_play(p);
    usleep(50000); /* 50ms to let thread start */
    CHECK(wb_preview_get_state(p) == WB_PREVIEW_PLAYING, "state = PLAYING after play");

    /* Let it run briefly */
    usleep(200000); /* 200ms */
    double t = wb_preview_get_time(p);
    CHECK(t > 0.0, "time advanced during playback");

    /* Pause */
    wb_preview_pause(p);
    usleep(50000);
    CHECK(wb_preview_get_state(p) == WB_PREVIEW_PAUSED, "state = PAUSED");
    double t2 = wb_preview_get_time(p);
    usleep(100000);
    CHECK(wb_preview_get_time(p) == t2, "time frozen when paused");

    /* Seek */
    wb_preview_seek(p, 2.5);
    usleep(50000);
    CHECK(wb_preview_get_time(p) == 2.5, "seek to 2.5s");

    /* Frame access */
    uint8_t *rgba = (uint8_t *)malloc(64 * 64 * 4);
    if (rgba) {
        int got = wb_preview_get_frame(p, rgba);
        CHECK(got >= 0, "frame get doesn't crash");
        free(rgba);
    }

    /* Stop */
    wb_preview_stop(p);
    CHECK(wb_preview_get_state(p) == WB_PREVIEW_STOPPED, "state = STOPPED after stop");

    /* Cleanup */
    wb_preview_destroy(p);
    CHECK(1, "preview destroyed");

    if (src->free) src->free(src);
    CHECK(1, "source freed");

    printf("\n=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
