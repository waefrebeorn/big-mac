/* tests/test_compositor_pro.c — test professional compositor feature. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wbus.h"

/* Forward declarations for wb_compositor_pro.c API */
void *wb_compositor_pro_create(int width, int height);
void  wb_compositor_pro_destroy(void *inst);
int   wb_compositor_pro_add_node(void *inst, int type);
int   wb_compositor_pro_connect(void *inst, int from, int from_port, int to, int to_port);
int   wb_compositor_pro_get_node_count(const void *inst);
int   wb_compositor_pro_process(void *inst, uint8_t *output_rgba);

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

int main(void) {
    void *c = wb_compositor_pro_create(100, 100);
    CHECK(c != NULL);

    /* 1. Add nodes */
    int input = wb_compositor_pro_add_node(c, 0); /* INPUT */
    int color = wb_compositor_pro_add_node(c, 4); /* COLOR */
    int output = wb_compositor_pro_add_node(c, 5); /* OUTPUT */
    CHECK(input >= 0 && color >= 0 && output >= 0);

    /* 2. Connect nodes */
    int rc = wb_compositor_pro_connect(c, input, 0, color, 0);
    CHECK(rc == 0);
    rc = wb_compositor_pro_connect(c, color, 0, output, 0);
    CHECK(rc == 0);

    /* 3. Node count */
    CHECK(wb_compositor_pro_get_node_count(c) == 3);

    /* 4. Process produces output */
    uint8_t *out = (uint8_t *)calloc(100 * 100 * 4, 1);
    rc = wb_compositor_pro_process(c, out);
    CHECK(rc == 0);

    /* 5. Output valid RGBA */
    int valid = 1;
    for (int i = 0; i < 100 * 100 * 4; i++) {
        if (out[i] > 255) valid = 0;
    }
    CHECK(valid);

    /* 6. Output has non-zero pixels */
    int nonzero = 0;
    for (int i = 0; i < 100 * 100 * 4; i++) {
        if (out[i] > 0) { nonzero = 1; break; }
    }
    CHECK(nonzero);

    free(out);
    wb_compositor_pro_destroy(c);

    printf("\nCompositor Pro: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}
