/* test_anim2.c — R055c verification: easing curves, camera keys, parenting. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_anim.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

static const int W = 320, H = 240;
static float centroid_x(const uint8_t *img) {
    long sx = 0; int n = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (img[(y*W+x)*4+3] == 255) { sx += x; n++; }
    return n ? (float)sx/n : -1;
}

int main(void) {
    printf("=== Anim R055c: easing / camera / parenting ===\n\n");
    static uint8_t img[W*H*4];

    /* ---- EASING: smoothstep vs linear vs bounce differ mid-segment ---- */
    float cx_lin = -1, cx_smooth = -1, cx_bounce = -1;
    const char *names[3] = { "linear", "smooth", "bounce" };
    for (int variant = 0; variant < 3; variant++) {
        wb_anim *a = wb_anim_create(W, H);
        wb_mesh *box = wb_mesh_box(0.5f, 0.5f, 0.5f, 255, 120, 60);
        wb_anim_add_object(a, box, 255, 120, 60);
        wb_anim_key_ease(a, 0, 0.0, -3, 0, 0, 0,0,0, 1, 0);
        wb_anim_key_ease(a, 0, 2.0,  3, 0, 0, 0,0,0, 1, variant==0?0:(variant==1?1:2));
        memset(img, 0, sizeof(img));
        wb_anim_render_frame(a, 0.5, img);   /* quarter-way in time */
        float cx = centroid_x(img);
        if (variant==0) cx_lin = cx;
        if (variant==1) cx_smooth = cx;
        if (variant==2) cx_bounce = cx;
        printf("         %-7s t=0.5 cx=%.1f\n", names[variant], cx);
        wb_mesh_free(box); wb_anim_free(a);
    }
    /* smoothstep at 25% time is ~16% distance -> LEFT of linear's 25% */
    CHECK(cx_smooth < cx_lin, "smoothstep lags linear early in segment");
    /* bounce at 25% has already overshot past linear (bounce-out is fast early) */
    CHECK(cx_bounce > cx_lin, "bounce-out leads linear early in segment");

    /* ---- CAMERA: keyframed dolly changes framing ---- */
    wb_anim *a = wb_anim_create(W, H);
    wb_mesh *box = wb_mesh_box(1,1,1, 200,60,60);
    wb_anim_add_object(a, box, 200,60,60);
    wb_anim_key(a, 0, 0.0, 0,0,0, 0,0,0, 1);
    CHECK(wb_anim_key_camera(a, 0.0, 0.3f, 0.5f, 5.0f) == 0, "camera key near");
    CHECK(wb_anim_key_camera(a, 2.0, 0.3f, 0.5f, 14.0f) == 0, "camera key far");
    memset(img, 0, sizeof(img));
    wb_anim_render_frame(a, 0.0, img);
    int px_near = 0;
    for (int i = 0; i < W*H; i++) if (img[i*4+3]==255) px_near++;
    memset(img, 0, sizeof(img));
    wb_anim_render_frame(a, 2.0, img);
    int px_far = 0;
    for (int i = 0; i < W*H; i++) if (img[i*4+3]==255) px_far++;
    printf("         cam near=%d px far=%d px\n", px_near, px_far);
    CHECK(px_near > px_far * 2, "camera dolly-out shrinks the object");

    /* ---- PARENTING: child inherits parent's translation ---- */
    wb_anim *b = wb_anim_create(W, H);
    wb_mesh *car = wb_mesh_box(0.8f, 0.3f, 0.5f, 60, 160, 60);
    wb_mesh *wheel = wb_mesh_cylinder(0.25f, 0.15f, 8, 40, 40, 40);
    int p = wb_anim_add_object(b, car, 60, 160, 60);
    int c = wb_anim_add_object(b, wheel, 40, 40, 40);
    wb_anim_key(b, p, 0.0, -3, 0, 0, 0,0,0, 1);
    wb_anim_key(b, p, 2.0,  3, 0, 0, 0,0,0, 1);
    wb_anim_key(b, c, 0.0,  0.5f, -0.3f, 0, 0,0,0, 1);   /* local offset only */
    CHECK(wb_anim_parent(b, c, p) == 0, "parent assigned");
    memset(img, 0, sizeof(img));
    wb_anim_render_frame(b, 0.0, img);
    float cx_at0 = centroid_x(img);
    memset(img, 0, sizeof(img));
    wb_anim_render_frame(b, 2.0, img);
    float cx_at2 = centroid_x(img);
    printf("         parent+child cx: t0=%.1f t2=%.1f\n", cx_at0, cx_at2);
    CHECK(cx_at0 < W*0.45f && cx_at2 > W*0.55f,
          "child rides the parent across the frame");
    /* invalid parent rejected */
    CHECK(wb_anim_parent(b, p, c) == -1, "child-as-parent rejected");

    wb_mesh_free(wheel); wb_mesh_free(car); wb_mesh_free(box);
    wb_anim_free(a); wb_anim_free(b);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
