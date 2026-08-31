/* tests/test_transitions_pro.c — test video transition pack. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wbus.h"

/* Local struct definition matching src/wb_transitions_pro.c */
typedef struct wb_transition {
    int type;
    int src_w, src_h;
    float duration;
    float param[4];
    int initialized;
} wb_transition;

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

int main(void) {
    wb_transition t;
    int w = 100, h = 100;
    uint8_t *from = (uint8_t *)calloc(w * h * 4, 1);
    uint8_t *to = (uint8_t *)calloc(w * h * 4, 1);
    uint8_t *out = (uint8_t *)calloc(w * h * 4, 1);

    /* Create test frames */
    for (int i = 0; i < w * h; i++) {
        from[i*4] = 255; from[i*4+1] = 0; from[i*4+2] = 0; from[i*4+3] = 255;
        to[i*4] = 0; to[i*4+1] = 0; to[i*4+2] = 255; to[i*4+3] = 255;
    }

    /* 1. Init/destroy */
    int rc = wb_transition_init(&t, 0, w, h);
    CHECK(rc == 0);

    /* 2. Dissolve at 0.5 = 50/50 blend */
    rc = wb_transition_process(&t, from, to, out, 0.5f);
    CHECK(rc == 0);
    CHECK(out[0] > 100 && out[0] < 200); /* R should be ~127 */
    CHECK(out[2] > 100 && out[2] < 200); /* B should be ~127 */

    /* 3. Wipe direction */
    t.type = 1; /* WIPE */
    t.param[0] = 0; /* left->right */
    rc = wb_transition_process(&t, from, to, out, 0.5f);
    CHECK(rc == 0);
    /* At progress 0.5 with dir=0, left half should be to (blue), right half from (red) */
    int mid = w / 2;
    CHECK(out[(w - mid / 2) * 4] > 200); /* Right side should be from (red) */
    CHECK(out[mid / 2 * 4 + 2] > 200); /* Left side should be to (blue) */

    /* 4. Glitch produces different output */
    t.type = 5; /* GLITCH */
    memset(out, 0, w * h * 4);
    rc = wb_transition_process(&t, from, to, out, 0.5f);
    CHECK(rc == 0);
    int different = 0;
    for (int i = 0; i < w * h * 4; i++) {
        if (out[i] != from[i] || out[i] != to[i]) { different = 1; break; }
    }
    CHECK(different);

    /* 5. All transition types init without crash */
    int all_ok = 1;
    for (int type = 0; type < 20; type++) {
        wb_transition tt;
        if (wb_transition_init(&tt, type, w, h) != 0) all_ok = 0;
    }
    CHECK(all_ok);

    /* 6. Output valid RGBA */
    t.type = 0; /* DISSOLVE */
    rc = wb_transition_process(&t, from, to, out, 0.5f);
    CHECK(rc == 0);
    int valid = 1;
    for (int i = 0; i < w * h * 4; i++) {
        if (out[i] > 255) valid = 0;
    }
    CHECK(valid);

    free(from); free(to); free(out);
    printf("\nTransitions Pro: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}
