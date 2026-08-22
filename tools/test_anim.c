/* test_anim.c — verification of the R054 keyframed animation layer.
 * Proves: sampling interpolates, objects composite into one frame,
 * animation actually moves pixels between keyframes, duration computes. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_anim.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

static const int W = 320, H = 240;

/* horizontal centroid of drawn pixels (x motion detection) */
static float centroid_x(const uint8_t *img) {
    long sx = 0; int n = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (img[(y*W+x)*4+3] == 255) { sx += x; n++; }
    return n ? (float)sx/n : -1;
}

int main(void) {
    printf("=== Animation layer (R054) test ===\n\n");

    wb_anim *a = wb_anim_create(W, H);
    CHECK(a != NULL, "anim created");
    if (!a) return 1;

    /* one cube sliding left->right over 2s */
    wb_mesh *box = wb_mesh_box(0.6f, 0.6f, 0.6f, 255, 100, 40);
    CHECK(wb_anim_add_object(a, box, 255, 100, 40) == 0, "object added");
    CHECK(wb_anim_object_count(a) == 1, "object count 1");

    CHECK(wb_anim_key(a, 0, 0.0, -3, 0, 0, 0,0,0, 1) == 0, "key at t=0 (left)");
    CHECK(wb_anim_key(a, 0, 2.0,  3, 0, 0, 0,0,0, 1) == 0, "key at t=2 (right)");

    CHECK(fabs(wb_anim_duration(a) - 2.0) < 1e-9, "duration = 2.0s");

    static uint8_t img[W*H*4];

    /* t=0: box at far LEFT -> centroid well below screen center */
    memset(img, 0, sizeof(img));
    wb_anim_render_frame(a, 0.0, img);
    float cx0 = centroid_x(img);
    CHECK(cx0 > 0 && cx0 < W*0.45f, "frame t=0: object on the LEFT");
    printf("         t=0   cx=%.1f\n", cx0);

    /* t=2: box at far RIGHT */
    memset(img, 0, sizeof(img));
    wb_anim_render_frame(a, 2.0, img);
    float cx2 = centroid_x(img);
    CHECK(cx2 > W*0.55f, "frame t=2: object on the RIGHT");
    printf("         t=2   cx=%.1f\n", cx2);

    /* t=1: midpoint — linear interp puts it near center */
    memset(img, 0, sizeof(img));
    wb_anim_render_frame(a, 1.0, img);
    float cx1 = centroid_x(img);
    CHECK(fabsf(cx1 - W*0.5f) < 30.0f, "frame t=1: object interpolated to center");
    printf("         t=1   cx=%.1f\n", cx1);

    /* monotonic sweep proves actual MOTION across frames */
    CHECK(cx0 < cx1 && cx1 < cx2, "centroid sweeps left->right (real motion)");

    /* hold beyond last key (t=3 same as t=2) */
    memset(img, 0, sizeof(img));
    wb_anim_render_frame(a, 3.5, img);
    float cxh = centroid_x(img);
    CHECK(fabsf(cxh - cx2) < 8.0f, "past-last-key holds final pose");

    /* two-object scene: second object offset in z renders with the first */
    wb_mesh *ball = wb_mesh_sphere(0.5f, 8, 10, 60, 120, 255);
    int o2 = wb_anim_add_object(a, ball, 60, 120, 255);
    CHECK(o2 == 1, "second object added");
    wb_anim_key(a, o2, 0.0, 0, 1.5f, -2, 0,0,0, 1);
    wb_anim_key(a, o2, 2.0, 0, 1.5f, -2, 6.283f,0,0, 1);   /* spins a turn */
    memset(img, 0, sizeof(img));
    wb_anim_render_frame(a, 1.0, img);
    int nboth = 0, nblue = 0;
    for (int i = 0; i < W*H; i++)
        if (img[i*4+3] == 255) { nboth++; if (img[i*4+2] > 100 && img[i*4+2] >= img[i*4]) nblue++; }
    CHECK(nboth > 2500, "both objects render into one frame");
    CHECK(nblue > 300, "sphere (blue) visible alongside cube");

    /* empty anim renders transparent (compositor-safe) */
    wb_anim *empty = wb_anim_create(64, 64);
    uint8_t tiny[64*64*4];
    memset(tiny, 0xFF, sizeof(tiny));
    wb_anim_render_frame(empty, 0.0, tiny);
    CHECK(tiny[3] == 0, "no-keys frame stays fully transparent");

    wb_mesh_free(ball); wb_mesh_free(box);
    wb_anim_free(empty);
    wb_anim_free(a);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
