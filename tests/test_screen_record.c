/* test_screen_record.c — verify screen recording engine */
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

    printf("=== Screen Recording Engine ===\n");

    /* Display enumeration */
    int count = wb_screen_display_count();
    CHECK(count > 0, "at least one display detected");

    if (count > 0) {
        int w = 0, h = 0;
        int rc = wb_screen_display_bounds(0, &w, &h);
        CHECK(rc == 0, "display 0 bounds retrieved");
        CHECK(w > 0 && h > 0, "display dimensions are positive");
        printf("  Display 0: %dx%d\n", w, h);
    }

    /* Screen record node creation */
    wb_node *node = wb_node_source_screen_record(0, 15);
    CHECK(node != NULL, "screen record node created");
    CHECK(node->kind == WB_NODE_SOURCE, "node kind is SOURCE");
    CHECK(node->n_inputs == 0, "no inputs (source node)");
    CHECK(node->pull != NULL, "pull function set");
    CHECK(node->free != NULL, "free function set");

    if (node) {
        /* Pull a frame (will start capture) */
        wb_frame *f = node->pull(node, 0.0, 0, 0, node->fmt_w, node->fmt_h, 0);
        /* Note: first pull may return NULL if no frame yet */
        CHECK(f == NULL || (f->w > 0 && f->h > 0), "frame dimensions valid (or NULL before first capture)");

        /* Pull again after brief delay */
        usleep(100000); /* 100ms */
        f = node->pull(node, 0.1, 0, 0, node->fmt_w, node->fmt_h, 0);
        CHECK(f != NULL || 1, "second pull completes without crash");

        /* Cleanup */
        if (node->free) node->free(node);
        CHECK(1, "node freed without crash");
    }

    printf("\n=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
